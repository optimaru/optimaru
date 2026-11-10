// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SOLVER_HPP
#define OPTIMARU_SOLVER_HPP

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/score/chain_objective.hpp"
#include "optimaru/score/objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/model/chain_view.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/construct/construction.hpp"
#include "optimaru/model/neighborhoods.hpp"
#include "optimaru/search/local_search.hpp"
#include "optimaru/accept/acceptors.hpp"
#include "optimaru/stop/stop_condition.hpp"
#include "optimaru/worker_threads.hpp"
#include "optimaru/search/operators.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Solver
// ---------------------------------------------------------------------------
// Runs one solve:
//   1. construction: everything unassigned gets its best placement (first
//      fit / first fit decreasing / cheapest insertion), see
//      construct/construction.hpp;
//   2. local search: one sampled move per step judged by an Acceptor, with
//      the built-in strategies and optional ruin-and-recreate kicks, see
//      search/local_search.hpp;
// plus the nearby-neighbour precomputation, custom operator registration and
// multi-start solving.
//
//   optimaru::Solver<MySolution> solver(settings);
//   optimaru::Score best = solver.solve(solution, objective);
//
// The engine sees the solution only through SolutionTraits<Solution>
// (variable access) and the Objective (scoring); the constraints, and every
// other piece of domain knowledge, live in the use case.
template <typename Solution>
class Solver {
    using Traits = SolutionTraits<Solution>;

public:
    Solver() = default;
    explicit Solver(Settings config) : config_(std::move(config)) {}

    // --- extension points for operator registration ---

    // Register a custom local-search move. Selection between the built-in
    // move mix and every registered operator is adaptive (roulette wheel,
    // rewarded by move outcome with the ruin_recreate rewards/decay scheme).
    // A custom move must keep every entity/element assigned.
    void add_move_operator(std::unique_ptr<MoveOperator<Solution>> op) {
        move_operators_.push_back(std::move(op));
    }

    // Register a custom LNS ruin operator; it joins the built-in
    // random/related/worst ruins under the same adaptive selection.
    void add_ruin_operator(std::unique_ptr<RuinOperator<Solution>> op) {
        ruin_operators_.push_back(std::move(op));
    }

    // Replace the configured AcceptanceRule with a custom acceptor; called
    // once per solve (and per ruin-and-recreate kick) with the current score.
    void set_acceptor_factory(std::function<std::unique_ptr<Acceptor>(const Score&)> factory) {
        acceptor_factory_ = std::move(factory);
    }

    // Register derived state (see DerivedState): the Editor updates it around
    // every write to a decision variable, so values computed from the
    // decision variables stay consistent for the objective.
    void add_derived_state(std::unique_ptr<DerivedState<Solution>> derived) {
        derived_states_.push_back(std::move(derived));
    }

    // Cooperatively stops a running solve() (checked once per step); the
    // solve returns its best-found solution as usual. Thread-safe. The flag
    // is sticky and solve() never clears it (a request_stop() racing a
    // starting solve would be lost otherwise), so a stopped solver stays
    // stopped: make a new Solver to solve again.
    void request_stop() { stop_requested_->store(true); }
    bool stop_requested() const { return stop_requested_->load(); }

    // Solves in place; returns the best score found.
    Score solve(Solution& solution, Objective<Solution>& objective) {
        static_assert(Traits::shape != Shape::Chain,
                      "chain-shaped solutions solve through the "
                      "ChainObjective overload");
        require_pinned_values<Traits>(solution);
        Editor<Solution> editor(solution, objective, config_.score_checks,
                                derived_states_.empty() ? nullptr : &derived_states_);
        StopCondition stop(config_.limits, stop_requested_.get(), run_stop_.get());
        build_nearby(solution);
        Construction<Solution>::run(editor, config_.construction);
        LocalSearch<Solution> local_search(config_, nearby_, move_operators_, ruin_operators_,
                                           acceptor_factory_);
        return local_search.run(editor, stop);
    }

    // Chain shape: materialize the chains into a list view, run the full
    // sequence machinery on it, and keep the domain's previous pointers in sync
    // through every move (see ChainView). Custom move/ruin
    // operators are not forwarded into the chained inner solve; derived
    // state and all settings (nearby, LNS, acceptors, limits) are.
    Score solve(Solution& solution, ChainObjective<Solution>& objective) {
        static_assert(Traits::shape == Shape::Chain, "this overload is for chain-shaped solutions");
        ChainView<Solution> working(solution);
        ChainObjectiveBridge<Solution> bridge(objective);
        Solver<ChainView<Solution>> inner(config_);
        inner.share_stop_flag(stop_requested_);
        inner.run_stop_ = run_stop_;
        if (acceptor_factory_) inner.set_acceptor_factory(acceptor_factory_);
        for (const auto& derived : derived_states_) {
            inner.add_derived_state(std::make_unique<ChainDerivedStateBridge<Solution>>(*derived));
        }
        return inner.solve(working, bridge);
    }

    // Shares a cooperative-stop flag with this solver: setting it has the
    // effect of request_stop(), and request_stop() sets it. The chain
    // facade hands its flag to its inner solver this way, and
    // solve_parallel_starts (solver_config.hpp) gives every start's solver
    // one flag, which the first start that fails raises so the others stop at
    // their next step.
    void share_stop_flag(std::shared_ptr<std::atomic<bool>> flag) {
        stop_requested_ = std::move(flag);
    }

    // Multi-start solving: config_.parallel_starts independent solves on
    // copies of the solution (seeds seed + i), keeping the best.
    // make_objective() is invoked once per start, on the calling thread.
    // Solution must be copy-constructible and copy-assignable.
    // A start that reaches Limits::target_score ends the run: the other
    // starts stop at their next step and hand over their best so far, and
    // the best of all starts wins (the lowest start on ties). Which start
    // gets there first depends on the threads, so with a reachable target
    // two runs with the same seed can return different solutions.
    // Exception-safe: an exception thrown inside a start (a raising
    // on_new_best callback, a ScoreChecks::Full score corruption, bad_alloc) is
    // caught on its worker thread, the other starts are told to stop at
    // their next step, every thread is joined, and the first exception is
    // rethrown on the calling thread (the solution is left as it was). A
    // thread that cannot be created (std::system_error) stops and joins the
    // starts already running before the error is rethrown
    // (run_worker_threads). request_stop() on this solver reaches every start.
    // Note: registered custom operators and derived states are per-Solver
    // (possibly stateful), so multi-start threads run with the built-ins only
    // — do NOT combine solve_multistart() with an objective that reads derived
    // values (their derived state would be missing in the threads). The
    // acceptor factory (set_acceptor_factory) IS shared: every start calls it
    // for an acceptor of its own, so it must be safe to call concurrently.
    template <typename ObjectiveFactory>
    Score solve_multistart(Solution& solution, ObjectiveFactory&& make_objective) {
        require_pinned_values<Traits>(solution);
        const int starts = std::max(1, config_.parallel_starts);
        if (starts == 1) {
            auto objective = make_objective();
            return solve(solution, *objective);
        }

        std::vector<Solution> copies(starts, solution);
        std::vector<Score> scores(starts);
        using ObjectivePtr = decltype(make_objective());
        std::vector<ObjectivePtr> objectives;
        objectives.reserve(starts);
        for (int i = 0; i < starts; ++i) objectives.push_back(make_objective());

        // Raised by the first start that fails or reaches the target; every
        // start checks it once a step (StopCondition's stop flag).
        auto stop = std::make_shared<std::atomic<bool>>(false);
        std::mutex failure_mutex;
        std::exception_ptr first_failure;
        run_worker_threads(
                starts, *stop,
                [this, &copies, &scores, &objectives, &stop, &failure_mutex,
                 &first_failure](int i) {
                    try {
                        Settings thread_config = config_;
                        thread_config.seed = config_.seed + static_cast<std::uint64_t>(i);
                        thread_config.parallel_starts = 1;
                        if (i != 0) thread_config.on_new_best = nullptr;  // only thread 0 reports
                        Solver thread_solver(std::move(thread_config));
                        if (acceptor_factory_)
                            thread_solver.set_acceptor_factory(acceptor_factory_);
                        thread_solver.share_stop_flag(stop_requested_);
                        thread_solver.run_stop_ = stop;
                        scores[i] = thread_solver.solve(copies[i], *objectives[i]);
                        if (scores[i] >= config_.limits.target_score) stop->store(true);
                    } catch (...) {
                        {
                            std::lock_guard<std::mutex> lock(failure_mutex);
                            if (!first_failure) first_failure = std::current_exception();
                        }
                        stop->store(true);
                    }
                });
        if (first_failure) std::rethrow_exception(first_failure);

        int best_index = 0;
        for (int i = 1; i < starts; ++i) {
            if (scores[i] > scores[best_index]) best_index = i;
        }
        solution = copies[best_index];
        return scores[best_index];
    }

private:
    // The chain facade hands its run-scoped stop flag to its inner solver.
    template <typename>
    friend class Solver;

    Settings config_;
    Neighborhoods nearby_;
    std::vector<std::unique_ptr<MoveOperator<Solution>>> move_operators_;
    std::vector<std::unique_ptr<RuinOperator<Solution>>> ruin_operators_;
    std::vector<std::unique_ptr<DerivedState<Solution>>> derived_states_;
    std::function<std::unique_ptr<Acceptor>(const Score&)> acceptor_factory_;
    // shared_ptr so the solver object stays movable.
    std::shared_ptr<std::atomic<bool>> stop_requested_ = std::make_shared<std::atomic<bool>>(false);
    // Run-scoped stop of one multi-start start (null outside solve_multistart).
    std::shared_ptr<std::atomic<bool>> run_stop_;

    void build_nearby(const Solution& solution) {
        const NeighborSettings& nearby_config = config_.search.nearby;
        if (!nearby_config.distance) return;
        int n = 0;
        if constexpr (Traits::shape == Shape::Assignment) {
            n = Traits::entity_count(solution);
        } else if constexpr (Traits::shape == Shape::Sequence) {
            n = Traits::element_count(solution);
        }
        nearby_.build(n, nearby_config.size, nearby_config.distance);
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_SOLVER_HPP

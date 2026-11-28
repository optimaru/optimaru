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
#include <random>
#include <utility>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/score/objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/model/element_index.hpp"
#include "optimaru/model/snapshot.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/construct/construction.hpp"
#include "optimaru/model/neighborhoods.hpp"
#include "optimaru/search/accept/acceptors.hpp"
#include "optimaru/search/accept/history_stages.hpp"
#include "optimaru/search/moves/move_mix.hpp"
#include "optimaru/search/operators.hpp"
#include "optimaru/search/ruin_recreate/greedy_repair.hpp"
#include "optimaru/search/ruin_recreate/kick.hpp"
#include "optimaru/search/ruin_recreate/ruin_pool.hpp"
#include "optimaru/search/select/schemes.hpp"
#include "optimaru/search/strategies.hpp"
#include "optimaru/stop/stop_condition.hpp"
#include "optimaru/worker_threads.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Solver
// ---------------------------------------------------------------------------
// Runs one solve:
//   1. construction: everything unassigned gets its best placement (first
//      fit / first fit decreasing / cheapest insertion), see
//      construct/construction.hpp;
//   2. the search (run() below): one candidate move per step, judged by an
//      Acceptor, until the StopCondition fires. A step's move is a built-in
//      strategy (search/strategies.hpp), one of the caller's operators or a
//      move of the ordinary mix (search/moves/move_mix.hpp); when the search
//      stalls, a ruin-and-recreate kick (search/ruin_recreate/kick.hpp) or
//      the next late-acceptance stage (search/accept/history_stages.hpp)
//      takes the step;
// plus the nearby-neighbour precomputation, custom operator registration and
// multi-start solving.
//
// docs/how-it-solves.md follows run() step by step, with where each part is
// and how to add one; the steps are marked below as "How it solves, <n>".
//
// The loop is the solver's own and holds no move: every part it draws on is
// a plugin under search/ (moves/, ruin_recreate/, accept/, select/), the
// built-in ones through the same MoveOperator / RuinOperator / Acceptor
// interfaces a caller registers its own with.
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
    using Snapshot = SolutionSnapshot<Solution>;

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

    // Cooperatively stops a running solve() (checked once per step); the
    // solve returns its best-found solution as usual. Thread-safe. The flag
    // is sticky and solve() never clears it (a request_stop() racing a
    // starting solve would be lost otherwise), so a stopped solver stays
    // stopped: make a new Solver to solve again.
    void request_stop() { stop_requested_->store(true); }
    bool stop_requested() const { return stop_requested_->load(); }

    // Solves in place; returns the best score found.
    Score solve(Solution& solution, Objective<Solution>& objective) {
        require_pinned_values<Traits>(solution);
        Editor<Solution> editor(solution, objective, config_.run.score_checks);
        StopCondition stop(config_.stop, stop_requested_.get(), run_stop_.get());
        build_nearby(solution);
        // How it solves, 1: construction (docs/how-it-solves.md).
        Construction<Solution>::run(editor, config_.construction);
        return run(editor, stop);
    }

    // Shares a cooperative-stop flag with this solver: setting it has the
    // effect of request_stop(), and request_stop() sets it.
    // solve_parallel_starts (solver_config.hpp) gives every start's solver
    // one flag this way, which the first start that fails or reaches the
    // target raises so the others stop at their next step.
    void share_stop_flag(std::shared_ptr<std::atomic<bool>> flag) {
        stop_requested_ = std::move(flag);
    }

    // Multi-start solving: config_.run.parallel_starts independent solves on
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
    // Note: registered custom operators are per-Solver (possibly stateful), so
    // multi-start threads run with the built-ins only. The acceptor factory
    // (set_acceptor_factory) IS shared: every start calls it for an acceptor
    // of its own, so it must be safe to call concurrently.
    template <typename ObjectiveFactory>
    Score solve_multistart(Solution& solution, ObjectiveFactory&& make_objective) {
        require_pinned_values<Traits>(solution);
        const int starts = std::max(1, config_.run.parallel_starts);
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
                        thread_config.run.seed = config_.run.seed + static_cast<std::uint64_t>(i);
                        thread_config.run.parallel_starts = 1;
                        if (i != 0)
                            thread_config.run.on_new_best = nullptr;  // only thread 0 reports
                        Solver thread_solver(std::move(thread_config));
                        if (acceptor_factory_)
                            thread_solver.set_acceptor_factory(acceptor_factory_);
                        thread_solver.share_stop_flag(stop_requested_);
                        thread_solver.run_stop_ = stop;
                        scores[i] = thread_solver.solve(copies[i], *objectives[i]);
                        if (scores[i] >= config_.stop.target_score) stop->store(true);
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
    Settings config_;
    Neighborhoods nearby_;
    std::vector<std::unique_ptr<MoveOperator<Solution>>> move_operators_;
    std::vector<std::unique_ptr<RuinOperator<Solution>>> ruin_operators_;
    std::function<std::unique_ptr<Acceptor>(const Score&)> acceptor_factory_;
    // shared_ptr so the solver object stays movable.
    std::shared_ptr<std::atomic<bool>> stop_requested_ = std::make_shared<std::atomic<bool>>(false);
    // Run-scoped stop of one multi-start start (null outside solve_multistart).
    std::shared_ptr<std::atomic<bool>> run_stop_;
    // The draw of the custom operators' gate (custom_operators_get_a_turn).
    std::uniform_real_distribution<double> unit_{0.0, 1.0};

    // The search: construction has run, `editor` holds its solution. Leaves
    // the best solution found in place and returns its score.
    Score run(Editor<Solution>& editor, StopCondition& stop) {
        Solution& solution = editor.solution();
        if constexpr (Traits::shape == Shape::Assignment) {
            if (Traits::entity_count(solution) == 0 || Traits::value_count(solution) == 0) {
                return editor.score();
            }
            if (all_entities_pinned(solution)) {
                const Score score = editor.score();
                if (config_.run.on_new_best) config_.run.on_new_best(0, score);
                return score;
            }
        } else {
            if (Traits::list_count(solution) == 0 || count_list_elements(solution) == 0) {
                return editor.score();
            }
        }

        // The parts of this solve. sequence shape: element id -> (list,
        // position), kept in sync with every move so nearby targets and the
        // ruins work in O(1) per lookup.
        ElementIndex<Solution> index;
        if constexpr (Traits::shape == Shape::Sequence) index.build(solution);
        // Ruin & repair, shared by the stagnation kick and the strategies
        // that do the same thing as an ordinary move.
        GreedyRepair<Solution> repair(index, nearby_, config_.alns, config_.unassigned_list);
        RuinPool<Solution> ruins(config_.alns, ruin_operators_);
        MoveMixSampler<Solution> moves(config_, nearby_, index);
        BuiltinStrategies<Solution> strategies(config_, nearby_, index, repair, ruins);
        StagnationKick<Solution> kick(config_.alns, repair, ruins, moves);
        // Adaptive selection over {move mix, custom operators...}; only
        // consulted when custom move operators are registered.
        auto move_selector =
                make_operator_selection_scheme(config_.alns, 1 + move_operators_.size());
        OperatorContext<Solution> operator_context{editor, nearby_, index.lists(),
                                                   index.positions()};

        const AlnsTriggerSettings& trigger = config_.alns_trigger;  // when a kick runs
        // How it solves, 2: the current and the best solution.
        Score current_score = editor.score();
        Score best_score = current_score;
        Snapshot best = Snapshot::take(solution);
        if (config_.run.on_new_best) config_.run.on_new_best(0, best_score);

        std::unique_ptr<Acceptor> acceptor = new_acceptor(current_score);
        // Custom acceptors may consume the time gradient. Of the built-ins,
        // only simulated annealing does; avoid a duration conversion and
        // floating-point division on every move for the default LA path.
        const bool needs_time_gradient =
                static_cast<bool>(acceptor_factory_) ||
                config_.acceptance.rule == AcceptanceRule::SimulatedAnnealing;
        std::mt19937_64 rng(config_.run.seed);
        // Reused by caller-provided operators. Keeping it out of SampledMove
        // avoids constructing and destroying a std::vector on every built-in
        // move, which is the hot path for all use cases.
        std::vector<int> custom_entities;
        std::int64_t step = 0;
        std::int64_t steps_since_best_or_lns = 0;
        // The strategies that evaluate thousands of candidates inside one
        // move (the iterated local search's descent, the ejection chain's
        // repair) consult the stop condition between batches of evaluations, so
        // one move cannot run far past a time limit the step loop only reads
        // between moves. Only the clock-reading criteria matter here; the
        // step-based ones see the current step.
        const std::function<bool()> stop_requested = [&] {
            return stop.should_stop(step, best_score, /*force_time_check=*/true);
        };
        operator_context.stop_requested = &stop_requested;
        // Kick cadence. "Stagnated" must be judged relative to how often this
        // instance normally improves: on a large instance plain LA improves
        // every ~10^5-10^6 steps, so a fixed step trigger would kick mid-
        // progress and turn the search into a perpetual restart. Two guards:
        //  - dynamic base: 8x the EMA of recent steps-between-new-bests,
        //    never below the configured trigger;
        //  - exponential backoff: every kick that fails to reach a new best
        //    before the next trigger doubles the interval (capped so kicks
        //    never stop entirely); any new best resets it.
        std::int64_t lns_trigger = trigger.unimproved_step_trigger;
        const std::int64_t lns_trigger_cap = trigger.unimproved_step_trigger << 12;
        std::int64_t last_best_step = 0;
        std::int64_t ema_best_gap = 0;
        bool force_time_check = false;
        // Late acceptance in stages of growing history
        // (AcceptanceSettings::history_growth).
        HistoryStages stages(config_, acceptor_factory_ != nullptr, current_score, stop.progress());

        // How it solves, 3: the stop condition, at the head of every step.
        while (!stop.should_stop(step, best_score, force_time_check)) {
            force_time_check = false;
            std::int64_t patience = std::max(lns_trigger, 8 * ema_best_gap);
            // A stage has converged only when a whole history has gone by
            // without an improving move.
            if (stages.enabled()) patience = std::max(patience, stages.min_patience());
            // How it solves, 4: stalled? The next stage if one is due, else a
            // kick (5), else wait.
            const bool stalled = steps_since_best_or_lns >= patience;
            if (stalled && stages.enabled() &&
                stages.on_converged(stop.progress(), !trigger.enabled)) {
                acceptor = stages.start_next(stop.progress());
                steps_since_best_or_lns = 0;
            } else if (stalled && trigger.enabled) {
                // How it solves, 5: the ruin and recreate kick.
                current_score = kick.apply(operator_context, current_score, best_score, rng);
                force_time_check = true;
                // A perturbation kick becomes the new incumbent uncondition-
                // ally and the acceptor restarts fresh, so
                // the following small moves re-polish the kicked solution
                // instead of instantly rejecting it (best is snapshotted, so
                // a bad kick can never worsen the returned solution).
                // (In stages: with the history of the stage the search is in.)
                acceptor = stages.enabled() ? stages.acceptor(current_score)
                                            : new_acceptor(current_score);
                steps_since_best_or_lns = 0;
                lns_trigger = std::min(lns_trigger * 2, lns_trigger_cap);
            } else if (stalled && stages.enabled()) {
                // No kicks, and the next stage is not due: wait and ask again.
                // (Once a stage no longer fits in the budget none will: the
                // search goes on with the history it has.)
                steps_since_best_or_lns = 0;
            } else {
                // How it solves, 6-8: a built-in strategy, a use-case move or
                // the move mix (choose_move).
                const MoveChoice choice =
                        choose_move(strategies, *move_selector, rng, stages.in_later_stage());
                MoveOperator<Solution>* op = choice.op;
                const int selector_index = choice.selector_index;
                const bool custom = selector_index > 0;

                // The candidate: its score, the entities it touched (for the
                // acceptor's tabu memory), and whether it is in place.
                // `previewed`: scored from the objective's cached data without
                // touching the solution, and applied only if accepted. Most
                // moves are rejected, so this is where the apply/score/undo
                // cost of a step disappears.
                SampledMove move;  // the move mix's own move
                bool previewed = false;
                bool applied = true;  // false: the operator found no move
                Score candidate_score;
                const int* entities = move.entities;
                int entity_count = 0;
                editor.begin_trial();
                if (op == nullptr) {
                    previewed = moves.sample(editor, rng, move, candidate_score);
                    entity_count = move.entity_count;
                } else {
                    if (custom) editor.track_touched_lists();
                    custom_entities.clear();
                    applied = op->apply(operator_context, rng, custom_entities);
                    if (applied) candidate_score = editor.score();
                    entities = custom_entities.data();
                    entity_count = static_cast<int>(custom_entities.size());
                }
                // How it solves, 9: the candidate's score, incremental or a
                // preview (above). How it solves, 10: the acceptor.
                if (applied) {
                    const bool accepted = acceptor->accepts(
                            candidate_score, current_score, best_score, entities, entity_count,
                            needs_time_gradient ? stop.progress() : 0.0, rng);
                    const MoveOutcome outcome =
                            classify_outcome(candidate_score, current_score, best_score, accepted);
                    if (accepted) {
                        // In stages, stagnation counts from the last improving
                        // move: a stage starts far below the best. (Strictly
                        // improving, on the score as it is compared: a use case
                        // whose soft level carries rounding noise would see
                        // sideways moves as improving here and never stall.)
                        if (stages.counts_improving_moves() && candidate_score > current_score) {
                            steps_since_best_or_lns = -1;
                        }
                        current_score = candidate_score;
                        acceptor->on_accepted(entities, entity_count);
                        if (previewed) moves.apply_previewed(editor, move);
                    } else if (!previewed) {  // a previewed move was never applied
                        editor.begin_rollback();
                        if (op == nullptr) {
                            moves.undo(editor, move);
                        } else {
                            op->undo_last(operator_context);
                        }
                    }
                    editor.end_trial(accepted);
                    if (selector_index >= 0) move_selector->update(selector_index, outcome);
                    if (op != nullptr) op->outcome(outcome);
                } else {
                    editor.end_trial(false);
                }
                if (custom) {
                    // Resynchronize the element index with whatever lists the
                    // custom operator touched (apply and/or undo).
                    if constexpr (Traits::shape == Shape::Sequence) {
                        for (int l : editor.touched_lists()) index.reindex(solution, l);
                    }
                    editor.untrack_touched_lists();
                }
                // Built-in strategies and caller-provided operators may scan
                // a neighbourhood or run a repair. Do not defer the clock
                // sample after those potentially expensive steps.
                if (op != nullptr) force_time_check = true;
            }
            acceptor->on_step_end(current_score);

            // How it solves, 11: a new best.
            if (current_score > best_score) {
                best_score = current_score;
                Snapshot::capture(best, solution);  // reuses the snapshot's storage
                stop.on_improvement(step);
                stages.on_new_best(stop.progress());
                steps_since_best_or_lns = 0;
                lns_trigger = trigger.unimproved_step_trigger;
                const std::int64_t gap = step - last_best_step;
                // The gap that spans a stage's restart is the length of the
                // restart, not how often this search improves: it stays out
                // of the average the patience is read from.
                if (!stages.take_restart()) {
                    ema_best_gap = ema_best_gap == 0 ? gap : (3 * ema_best_gap + gap) / 4;
                }
                last_best_step = step;
                if (config_.run.on_new_best) config_.run.on_new_best(step, best_score);
            } else {
                ++steps_since_best_or_lns;
            }
            ++step;
        }

        Snapshot::restore(editor, best);
        return best_score;
    }

    // What one step tries: an operator, or the ordinary move mix (op == nullptr).
    // `selector_index` >= 0: the adaptive move selector was asked and hears the
    // outcome (0 is the move mix, i > 0 the caller's operator i - 1).
    struct MoveChoice {
        MoveOperator<Solution>* op = nullptr;
        int selector_index = -1;
    };

    // How a step chooses its move, in three stages and in this order:
    //  1. built-in strategies (BuiltinStrategies::draw): each by its own
    //     probability, in priority order; the first that hits is the move;
    //  2. the use case's own moves (custom_operators_get_a_turn): within their
    //     gate the adaptive selector picks one of them or the move mix;
    //  3. the move mix (MoveMixSampler::sample): one ordinary move, drawn by
    //     relative weight, its entity and value steered by the use case's hints.
    // The random draws happen in exactly this order; a change to it changes
    // every seeded result.
    MoveChoice choose_move(BuiltinStrategies<Solution>& strategies,
                           OperatorSelectionScheme& move_selector, std::mt19937_64& rng,
                           bool in_later_stage) {
        MoveChoice choice;
        choice.op = strategies.draw(rng);
        if (choice.op == nullptr && custom_operators_get_a_turn(rng, in_later_stage)) {
            choice.selector_index = move_selector.select(rng);
            if (choice.selector_index > 0) {
                choice.op =
                        move_operators_[static_cast<std::size_t>(choice.selector_index) - 1].get();
            }
        }
        return choice;
    }

    // Whether this step asks the adaptive selector to choose between the move
    // mix and the caller's operators (CustomMoveSettings::share).
    bool custom_operators_get_a_turn(std::mt19937_64& rng, bool in_later_stage) {
        if (move_operators_.empty()) return false;
        double gate = config_.custom_moves.share;
        if (in_later_stage) gate *= config_.acceptance.later_stage_custom_move_share;
        return gate >= 1.0 || (gate > 0.0 && unit_(rng) < gate);
    }

    // The acceptor a search starting at `initial_score` judges its moves by:
    // the caller's (set_acceptor_factory) or the configured rule.
    std::unique_ptr<Acceptor> new_acceptor(const Score& initial_score) const {
        if (acceptor_factory_) return acceptor_factory_(initial_score);
        return make_acceptor<Solution>(config_, initial_score);
    }

    // With nothing movable no move, built-in or custom, has a legal target, so
    // run() stops here rather than have every selection handle that case.
    static bool all_entities_pinned(const Solution& sol) {
        const int n = Traits::entity_count(sol);
        for (int e = 0; e < n; ++e)
            if (!entity_is_pinned<Traits>(sol, e)) return false;
        return true;
    }

    static int count_list_elements(const Solution& solution) {
        int total = 0;
        for (int l = 0; l < Traits::list_count(solution); ++l) {
            total += Traits::list_size(solution, l);
        }
        return total;
    }

    void build_nearby(const Solution& solution) {
        const NeighborSettings& nearby_config = config_.selection.nearby;
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

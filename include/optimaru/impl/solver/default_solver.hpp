#ifndef OPTIMARU_IMPL_SOLVER_DEFAULT_SOLVER_HPP
#define OPTIMARU_IMPL_SOLVER_DEFAULT_SOLVER_HPP

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"
#include "optimaru/api/score/calculator/chained_incremental_score_calculator.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/impl/domain/variable/chained_working_solution.hpp"
#include "optimaru/config/solver/solver_config.hpp"
#include "optimaru/impl/constructionheuristic/default_construction_heuristic_phase.hpp"
#include "optimaru/impl/heuristic/selector/common/nearby/nearby_neighborhoods.hpp"
#include "optimaru/impl/localsearch/default_local_search_phase.hpp"
#include "optimaru/impl/localsearch/decider/acceptor/acceptor.hpp"
#include "optimaru/impl/solver/termination/termination.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// DefaultSolver
// ---------------------------------------------------------------------------
// Mirrors impl.solver.DefaultSolver: orchestrates the phases —
//   1. DefaultConstructionHeuristicPhase (First Fit / FFD / cheapest insertion)
//   2. DefaultLocalSearchPhase (acceptor-driven moves + optional LNS kicks)
// — plus nearby-neighborhood precomputation, custom-operator registration and
// multi-start parallel solving.
//
// The engine only sees the solution through SolutionDescriptor<Solution>
// (variable access) and IncrementalScoreCalculator<Solution> (scoring); all
// domain knowledge — the constraints — lives in the quickstart.
template <typename Solution>
class DefaultSolver {
    using Descriptor = SolutionDescriptor<Solution>;

public:
    DefaultSolver() = default;
    explicit DefaultSolver(SolverConfig config) : config_(std::move(config)) {}

    // --- extension points for operator registration ---

    // Register a custom local-search move. Selection between the built-in
    // move mix and every registered operator is adaptive (roulette wheel,
    // rewarded by move outcome with the ruinRecreate rewards/decay scheme).
    // A custom move must keep every entity/element assigned.
    void addMoveOperator(std::unique_ptr<MoveOperator<Solution>> op) {
        moveOperators_.push_back(std::move(op));
    }

    // Register a custom LNS ruin operator; it joins the built-in
    // random/related/worst ruins under the same adaptive selection.
    void addRuinOperator(std::unique_ptr<RuinOperator<Solution>> op) {
        ruinOperators_.push_back(std::move(op));
    }

    // Replace the acceptorType enum with a custom acceptance strategy; called
    // once per solve (and per LNS kick) with the current incumbent score.
    void setAcceptorFactory(
            std::function<std::unique_ptr<Acceptor>(const HardSoftScore&)> factory) {
        acceptorFactory_ = std::move(factory);
    }

    // Register a shadow-variable listener (see VariableListener): invoked by
    // the ScoreDirector around every genuine-variable mutation so derived
    // values on the solution stay consistent for the score calculator.
    void addVariableListener(std::unique_ptr<VariableListener<Solution>> listener) {
        variableListeners_.push_back(std::move(listener));
    }

    // Cooperatively stops a running solve() (checked once per step); the
    // solve returns its best-found solution as usual. Thread-safe. The flag
    // is sticky: to reuse this solver instance afterwards, call
    // resetTerminateEarly() first (solve() itself must not clear it, or a
    // terminateEarly() racing a starting solve would be lost).
    void terminateEarly() { terminateEarly_->store(true); }
    bool isTerminateEarly() const { return terminateEarly_->load(); }
    void resetTerminateEarly() { terminateEarly_->store(false); }

    // Solves in place; returns the best score found.
    HardSoftScore solve(Solution& solution, IncrementalScoreCalculator<Solution>& calculator) {
        static_assert(Descriptor::kind != VariableKind::CHAINED,
                      "CHAINED solutions solve through the "
                      "ChainedIncrementalScoreCalculator overload");
        requirePinnedValues<Descriptor>(solution);
        ScoreDirector<Solution> director(solution, calculator, config_.environmentMode,
                                         variableListeners_.empty() ? nullptr
                                                                    : &variableListeners_);
        SolverTermination termination(config_.termination, terminateEarly_.get());
        buildNearby(solution);
        DefaultConstructionHeuristicPhase<Solution>::run(director, config_.constructionHeuristic);
        DefaultLocalSearchPhase<Solution> localSearch(config_, nearby_, moveOperators_,
                                                      ruinOperators_, acceptorFactory_);
        return localSearch.run(director, termination);
    }

    // CHAINED kind: materialize the chains into a list view, run the full
    // LIST machinery on it, and keep the domain's previous pointers in sync
    // through every move (see ChainedWorkingSolution). Custom move/ruin
    // operators are not forwarded into the chained inner solve; shadow
    // listeners and all config (nearby, LNS, acceptors, terminations) are.
    HardSoftScore solve(Solution& solution,
                        ChainedIncrementalScoreCalculator<Solution>& calculator) {
        static_assert(Descriptor::kind == VariableKind::CHAINED,
                      "this overload is for CHAINED solutions");
        ChainedWorkingSolution<Solution> working(solution);
        ChainedCalculatorBridge<Solution> bridge(calculator);
        DefaultSolver<ChainedWorkingSolution<Solution>> inner(config_);
        inner.adoptTerminateEarlyFlag(terminateEarly_);
        if (acceptorFactory_) inner.setAcceptorFactory(acceptorFactory_);
        for (const auto& listener : variableListeners_) {
            inner.addVariableListener(
                    std::make_unique<ChainedListenerBridge<Solution>>(*listener));
        }
        return inner.solve(working, bridge);
    }

    // Internal: lets a wrapping solver (the CHAINED facade) share its
    // cooperative-stop flag with the inner solver.
    void adoptTerminateEarlyFlag(std::shared_ptr<std::atomic<bool>> flag) {
        terminateEarly_ = std::move(flag);
    }

    // Multi-start solving: config_.multiStartCount independent solves on
    // copies of the solution (seeds randomSeed + i), keeping the best.
    // calculatorFactory() is invoked once per start, on the calling thread.
    // Solution must be copy-constructible and copy-assignable.
    // Note: registered custom operators and variable listeners are per-Solver
    // (possibly stateful), so multi-start threads run with the built-ins only
    // — do NOT combine solveMultiStart() with a calculator that reads shadow
    // variables (their listeners would be missing in the threads).
    template <typename CalculatorFactory>
    HardSoftScore solveMultiStart(Solution& solution, CalculatorFactory&& calculatorFactory) {
        requirePinnedValues<Descriptor>(solution);
        const int starts = std::max(1, config_.multiStartCount);
        if (starts == 1) {
            auto calculator = calculatorFactory();
            return solve(solution, *calculator);
        }

        std::vector<Solution> copies(starts, solution);
        std::vector<HardSoftScore> scores(starts);
        using CalculatorPtr = decltype(calculatorFactory());
        std::vector<CalculatorPtr> calculators;
        calculators.reserve(starts);
        for (int i = 0; i < starts; ++i) calculators.push_back(calculatorFactory());

        std::vector<std::thread> threads;
        threads.reserve(starts);
        for (int i = 0; i < starts; ++i) {
            threads.emplace_back([this, i, &copies, &scores, &calculators] {
                SolverConfig threadConfig = config_;
                threadConfig.randomSeed = config_.randomSeed + static_cast<std::uint64_t>(i);
                threadConfig.multiStartCount = 1;
                if (i != 0) threadConfig.onNewBest = nullptr;  // only thread 0 reports
                DefaultSolver threadSolver(std::move(threadConfig));
                scores[i] = threadSolver.solve(copies[i], *calculators[i]);
            });
        }
        for (std::thread& t : threads) t.join();

        int bestIndex = 0;
        for (int i = 1; i < starts; ++i) {
            if (scores[i] > scores[bestIndex]) bestIndex = i;
        }
        solution = copies[bestIndex];
        return scores[bestIndex];
    }

private:
    SolverConfig config_;
    NearbyNeighborhoods nearby_;
    std::vector<std::unique_ptr<MoveOperator<Solution>>> moveOperators_;
    std::vector<std::unique_ptr<RuinOperator<Solution>>> ruinOperators_;
    std::vector<std::unique_ptr<VariableListener<Solution>>> variableListeners_;
    std::function<std::unique_ptr<Acceptor>(const HardSoftScore&)> acceptorFactory_;
    // shared_ptr so the solver object stays movable (SolverManager jobs).
    std::shared_ptr<std::atomic<bool>> terminateEarly_ =
            std::make_shared<std::atomic<bool>>(false);

    void buildNearby(const Solution& solution) {
        const NearbySelectionConfig& nearbyConfig = config_.localSearch.nearby;
        if (!nearbyConfig.distanceMeter) return;
        int n = 0;
        if constexpr (Descriptor::kind == VariableKind::BASIC) {
            n = Descriptor::entityCount(solution);
        } else if constexpr (Descriptor::kind == VariableKind::LIST) {
            n = Descriptor::elementCount(solution);
        }
        nearby_.build(n, nearbyConfig.size, nearbyConfig.distanceMeter);
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_SOLVER_DEFAULT_SOLVER_HPP

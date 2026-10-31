#include "ks/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ks/construction.hpp"
#include "ks/incremental_calculator.hpp"
#include "ks/score_calculator.hpp"
#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"

namespace ks
{

namespace
{

void requireRewards(const Solver::Config &config)
{
    if (config.ruinRecreateRewards.size() != 4)
        throw std::invalid_argument("ruinRecreateRewards must contain exactly 4 values");
}

// The engine configuration shared by both solve() overloads: late-acceptance
// local search over the built-in BASIC moves, plus the ALNS kicks. The
// engine's construction heuristic finds nothing left to place: the domain
// construction decided every item.
optimaru::SolverConfig engineConfigOf(const Solver::Config &config,
                                      const KnapsackProblem &problem,
                                      int starts)
{
    optimaru::SolverConfig engineConfig;
    engineConfig.randomSeed = config.randomSeed;
    engineConfig.environmentMode = config.environmentMode;
    engineConfig.multiStartCount = starts;
    engineConfig.onNewBest = config.onNewBest;
    engineConfig.termination.spentLimit = config.spentLimit;
    engineConfig.termination.bestScoreLimit = config.bestScoreLimit;
    engineConfig.termination.unimprovedSpentLimit = config.unimprovedSpentLimit;
    engineConfig.termination.stepCountLimit = config.stepCountLimit;
    engineConfig.termination.unimprovedStepCountLimit = config.unimprovedStepCountLimit;
    engineConfig.localSearch.acceptor.lateAcceptanceSize
        = config.lateAcceptanceSize != 0
              ? config.lateAcceptanceSize
              : std::clamp<std::size_t>(8 * problem.items.size(), 400, 3200);
    engineConfig.localSearch.moveSelector.changeMoveWeight = config.changeMoveWeight;
    engineConfig.localSearch.moveSelector.swapMoveWeight = config.swapMoveWeight;
    engineConfig.localSearch.descentWeight = config.descentWeight;

    // Nearby swap partners: items of similar weight. The weights are copied,
    // so the meter stays valid in the multi-start worker threads, whose
    // problems are copies.
    if (config.nearbyProbability > 0.0 && config.nearbySize > 0)
    {
        auto weights = std::make_shared<std::vector<std::int64_t>>();
        for (const Item &item : problem.items)
            weights->push_back(item.weight);
        engineConfig.localSearch.nearby.probability = config.nearbyProbability;
        engineConfig.localSearch.nearby.size = config.nearbySize;
        engineConfig.localSearch.nearby.distanceMeter = [weights](int a, int b)
        { return static_cast<double>(std::llabs((*weights)[a] - (*weights)[b])); };
    }

    auto &lns = engineConfig.localSearch.ruinRecreate;
    lns.enabled = config.ruinRecreate;
    lns.unimprovedStepTrigger = config.ruinRecreateUnimprovedStepTrigger;
    lns.minRuinFraction = config.ruinRecreateMinRuinFraction;
    lns.maxRuinFraction = config.ruinRecreateMaxRuinFraction;
    lns.maxRuinCount = config.ruinRecreateMaxRuinCount;
    lns.operatorDecay = config.ruinRecreateOperatorDecay;
    lns.rewards = config.ruinRecreateRewards;
    lns.polishSteps = config.ruinRecreatePolishSteps;
    lns.asMoveWeight = config.ruinRecreateAsMoveWeight;
    lns.asMoveMaxRuin = config.ruinRecreateAsMoveMaxRuin;
    return engineConfig;
}

// Validates, constructs and consumes the seed.
void construct(KnapsackProblem &problem)
{
    ScoreCalculator::prepare(problem);  // validates the facts and the selection
    constructSelection(problem);
    problem.warmStart = false;  // a seed is used once
}

}  // namespace

void Solver::solve(KnapsackProblem &problem)
{
    requireRewards(config_);
    construct(problem);
    if (!problem.items.empty())
    {
        // Independent parallel starts through the engine's own multi-start:
        // no custom operator or variable listener is registered, so the
        // worker threads run exactly the same search (seed + i each).
        const int starts = std::max(1, config_.parallelSolverCount);
        optimaru::Solver<KnapsackProblem> engine(engineConfigOf(config_, problem, starts));
        engine.solveMultiStart(problem,
                               [] { return std::make_unique<KnapsackIncrementalScoreCalculator>(); });
    }
    // The authoritative final score: a full recompute on the best solution.
    ScoreCalculator::calculateScore(problem);
}

void Solver::solve(KnapsackProblem &problem,
                   optimaru::DecomposedScoreCalculator<KnapsackProblem> &calculator)
{
    requireRewards(config_);
    construct(problem);
    if (!problem.items.empty())
    {
        // Single-start: the caller-owned calculator cannot be cloned.
        optimaru::Solver<KnapsackProblem> engine(engineConfigOf(config_, problem, 1));
        engine.solve(problem, calculator);
    }
    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraintTotals() explains exactly this score.
    calculator.resetWorkingSolution(problem);
    problem.score = calculator.calculateScore();
}

}  // namespace ks

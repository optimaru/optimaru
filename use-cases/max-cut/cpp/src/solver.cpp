#include "mc/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "mc/incremental_calculator.hpp"
#include "mc/score_calculator.hpp"
#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"

namespace mc
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
// engine's construction heuristic finds nothing left to place: the start
// partition assigned every vertex.
optimaru::SolverConfig engineConfigOf(const Solver::Config &config, const MaxCutProblem &problem, int starts)
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
              : std::clamp<std::size_t>(8 * problem.vertexIds.size(), 400, 3200);
    engineConfig.localSearch.moveSelector.changeMoveWeight = config.changeMoveWeight;
    engineConfig.localSearch.moveSelector.swapMoveWeight = config.swapMoveWeight;
    engineConfig.localSearch.descentWeight = config.descentWeight;

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

    if (problem.exactSize())
    {
        // Swaps only: every other move (a flip, a ruin's unassign-and-recreate,
        // a descent's change) could change a side size.
        engineConfig.localSearch.moveSelector.changeMoveWeight = 0.0;
        engineConfig.localSearch.moveSelector.swapMoveWeight = 1.0;
        engineConfig.localSearch.descentWeight = 0.0;
        lns.enabled = false;
        lns.asMoveWeight = 0.0;
    }
    return engineConfig;
}

// Validates, builds the start partition and consumes the seed.
void construct(MaxCutProblem &problem)
{
    ScoreCalculator::prepare(problem);  // validates the facts and the partition
    if (!problem.warmStart)
        std::fill(problem.side.begin(), problem.side.end(), kUnassigned);
    for (int v = 0; v < problem.vertexCount(); ++v)
    {
        if (problem.side[v] == kUnassigned)
            problem.side[v] = problem.isPinned(v) ? problem.fixedSide[v] : v % kSideCount;
    }
    problem.warmStart = false;  // a seed is used once
}

// Whether the search has anything to do: an empty graph has no move, and in
// exact-size mode a swap needs two free vertices (the engine's fallback
// for fewer entities is a change move, which would break the size).
bool searchable(const MaxCutProblem &problem)
{
    if (problem.vertexCount() == 0)
        return false;
    return !problem.exactSize() || problem.movableCount() >= 2;
}

}  // namespace

void Solver::solve(MaxCutProblem &problem)
{
    requireRewards(config_);
    construct(problem);
    if (searchable(problem))
    {
        // Independent parallel starts through the engine's own multi-start:
        // no custom operator or variable listener is registered, so the
        // worker threads run exactly the same search (seed + i each).
        const int starts = std::max(1, config_.parallelSolverCount);
        optimaru::Solver<MaxCutProblem> engine(engineConfigOf(config_, problem, starts));
        engine.solveMultiStart(problem, [] { return std::make_unique<MaxCutIncrementalScoreCalculator>(); });
    }
    // The authoritative final score: a full recompute on the best solution.
    ScoreCalculator::calculateScore(problem);
}

void Solver::solve(MaxCutProblem &problem, optimaru::DecomposedScoreCalculator<MaxCutProblem> &calculator)
{
    requireRewards(config_);
    construct(problem);
    if (searchable(problem))
    {
        // Single-start: the caller-owned calculator cannot be cloned.
        optimaru::Solver<MaxCutProblem> engine(engineConfigOf(config_, problem, 1));
        engine.solve(problem, calculator);
    }
    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraintTotals() explains exactly this score.
    calculator.resetWorkingSolution(problem);
    problem.score = calculator.calculateScore();
    problem.scored = true;
}

}  // namespace mc

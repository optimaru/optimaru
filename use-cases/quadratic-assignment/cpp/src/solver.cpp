#include "qap/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"
#include "qap/incremental_calculator.hpp"
#include "qap/score_calculator.hpp"

namespace qap
{

namespace
{

// The engine configuration shared by both solve() overloads: swap moves only
// (changeMoveWeight 0 with a positive swap weight; the engine falls back to a
// change move only when BOTH are 0, so the swap weight stays 1), every
// value-assigning strategy off (their engine defaults are off already; set
// here so a future default cannot switch them on behind the permutation's
// back). The engine's construction heuristic finds nothing to place: the
// start is a complete permutation.
optimaru::SolverConfig engineConfigOf(const Solver::Config &config, const QuadraticAssignmentProblem &problem, int starts)
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

    auto &acceptor = engineConfig.localSearch.acceptor;
    acceptor.acceptorType = config.acceptorType;
    acceptor.lateAcceptanceSize = config.lateAcceptanceSize != 0
                                      ? config.lateAcceptanceSize
                                      : std::clamp<std::size_t>(8 * problem.facilities.size(), 400, 3200);
    acceptor.entityTabuSize = config.entityTabuSize;

    auto &local = engineConfig.localSearch;
    local.moveSelector.changeMoveWeight = 0.0;
    local.moveSelector.swapMoveWeight = 1.0;
    local.ruinRecreate.enabled = false;
    local.ruinRecreate.asMoveWeight = 0.0;
    local.descentWeight = 0.0;
    local.routeEliminationWeight = 0.0;
    local.ejectionChainWeight = 0.0;
    local.iteratedLocalSearchWeight = 0.0;
    local.valueRecommender = nullptr;
    local.entityRecommender = nullptr;
    return engineConfig;
}

void requireConfig(const Solver::Config &config)
{
    if (config.parallelSolverCount <= 0)
        throw std::invalid_argument("parallelSolverCount must be positive");
    if (config.entityTabuSize < 0)
        throw std::invalid_argument("entityTabuSize must be >= 0");
}

// Validates and lays out the start: the seed (a full bijection) once, else
// the identity. Returns false when there is nothing to search (n <= 1).
bool start(QuadraticAssignmentProblem &problem)
{
    ScoreCalculator::prepare(problem);  // validates the facts
    if (problem.warmStart)
    {
        if (!problem.isBijection())
        {
            throw std::invalid_argument(
                "warm start needs a full bijection: every facility at its own location");
        }
    }
    else
    {
        for (int f = 0; f < problem.facilityCount(); ++f)
            problem.facilities[f].location = f;
    }
    problem.warmStart = false;  // a seed is used once
    return problem.facilityCount() > 1;
}

void requireBijection(const QuadraticAssignmentProblem &problem)
{
    if (!problem.isBijection())
        throw std::logic_error("quadratic-assignment solve left a non-bijective assignment");
}

}  // namespace

void Solver::solve(QuadraticAssignmentProblem &problem)
{
    requireConfig(config_);
    if (start(problem))
    {
        // Independent parallel starts through the engine's own multi-start:
        // no custom operator or variable listener is registered, so the
        // worker threads run exactly the same search (seed + i each).
        const int starts = std::max(1, config_.parallelSolverCount);
        optimaru::Solver<QuadraticAssignmentProblem> engine(engineConfigOf(config_, problem, starts));
        engine.solveMultiStart(problem,
                               [] { return std::make_unique<QuadraticAssignmentIncrementalScoreCalculator>(); });
    }
    requireBijection(problem);
    // The authoritative final score: a full recompute on the best solution.
    ScoreCalculator::calculateScore(problem);
}

void Solver::solve(QuadraticAssignmentProblem &problem,
                   optimaru::DecomposedScoreCalculator<QuadraticAssignmentProblem> &calculator)
{
    requireConfig(config_);
    if (start(problem))
    {
        // Single-start: the caller-owned calculator cannot be cloned.
        optimaru::Solver<QuadraticAssignmentProblem> engine(engineConfigOf(config_, problem, 1));
        engine.solve(problem, calculator);
    }
    requireBijection(problem);
    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraintTotals() explains exactly this score.
    calculator.resetWorkingSolution(problem);
    problem.score = calculator.calculateScore();
}

}  // namespace qap

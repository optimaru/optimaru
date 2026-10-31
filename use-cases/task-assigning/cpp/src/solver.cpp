#include "ta/solver.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>

#include "ta/incremental_calculator.hpp"
#include "ta/score_calculator.hpp"
#include "optimaru/api/solver/solver_factory.hpp"
#include "optimaru/config/solver/solver_config.hpp"

namespace ta
{

namespace
{

// Translates a domain Config into the engine's SolverConfig: termination,
// late acceptance, the LIST move mix and the ALNS knobs. Shared by both
// solve() overloads.
optimaru::SolverConfig buildEngineConfig(const Solver::Config &config,
                                         const TaskAssigningSolution &solution,
                                         int multiStartCount)
{
    optimaru::SolverConfig engineConfig;
    engineConfig.randomSeed = config.randomSeed;
    engineConfig.environmentMode = config.environmentMode;
    engineConfig.multiStartCount = multiStartCount;
    if (config.onNewBest)
    {
        auto onNewBest = config.onNewBest;
        engineConfig.onNewBest =
            [onNewBest](std::int64_t step, const optimaru::HardSoftScore &score)
        { onNewBest(step, fromEngineScore(score)); };
    }
    engineConfig.termination.spentLimit = config.spentLimit;
    engineConfig.termination.bestScoreLimit
        = toEngineScore(config.bestScoreLimit);
    engineConfig.termination.unimprovedSpentLimit = config.unimprovedSpentLimit;
    engineConfig.termination.stepCountLimit = config.stepCountLimit;
    engineConfig.termination.unimprovedStepCountLimit
        = config.unimprovedStepCountLimit;
    // lateAcceptanceSize 0 = auto-scale with the instance (see Config).
    engineConfig.localSearch.acceptor.lateAcceptanceSize
        = config.lateAcceptanceSize != 0
              ? config.lateAcceptanceSize
              : std::clamp<std::size_t>(8 * solution.tasks.size(), 400, 3200);
    auto &moves = engineConfig.localSearch.moveSelector;
    moves.relocateMoveWeight = config.relocateMoveWeight;
    moves.listSwapMoveWeight = config.listSwapMoveWeight;
    moves.twoOptMoveWeight = config.twoOptMoveWeight;
    moves.orOptMoveWeight = config.orOptMoveWeight;
    moves.tailSwapMoveWeight = config.tailSwapMoveWeight;
    auto &lns = engineConfig.localSearch.ruinRecreate;
    lns.enabled = config.ruinRecreate;
    lns.unimprovedStepTrigger = config.ruinRecreateUnimprovedStepTrigger;
    lns.minRuinFraction = config.ruinRecreateMinRuinFraction;
    lns.maxRuinFraction = config.ruinRecreateMaxRuinFraction;
    lns.maxRuinCount = config.ruinRecreateMaxRuinCount;
    lns.operatorDecay = config.ruinRecreateOperatorDecay;
    lns.rewards = config.ruinRecreateRewards;
    lns.polishSteps = config.ruinRecreatePolishSteps;
    return engineConfig;
}

}  // namespace

void Solver::solve(TaskAssigningSolution &solution)
{
    if (solution.tasks.empty() || solution.employees.empty())
    {
        // Nothing to plan (or nobody to plan for): just score the input.
        ScoreCalculator::calculateScore(solution);
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }

    // Cheapest-insertion construction + late-acceptance local search over
    // the LIST moves (+ ALNS kicks), multi-start parallel when
    // parallelSolverCount > 1. Each start gets its own calculator instance
    // (the calculators are stateful).
    auto engine
        = optimaru::SolverFactory<TaskAssigningSolution>::create(
              buildEngineConfig(
                  config_, solution, std::max(1, config_.parallelSolverCount)))
              .buildSolver();
    engine.solveMultiStart(
        solution,
        []
        {
            return std::make_unique<TaskAssigningIncrementalScoreCalculator>();
        });

    // The authoritative final score — a full recompute that also re-derives
    // the task shadow variables (employee, startTime) on the best solution.
    ScoreCalculator::calculateScore(solution);
}

void Solver::solve(
    TaskAssigningSolution &solution,
    optimaru::DecomposedScoreCalculator<TaskAssigningSolution> &calculator)
{
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }
    if (!solution.tasks.empty() && !solution.employees.empty())
    {
        // Single-start: the caller-owned calculator cannot be cloned, so the
        // multi-start factory would have nothing to hand its extra workers.
        auto engine = optimaru::SolverFactory<TaskAssigningSolution>::create(
                          buildEngineConfig(config_, solution, 1))
                          .buildSolver();
        engine.solve(solution, calculator);
    }

    // Re-derive the task shadow variables (employee, startTime) on the best
    // solution, then score it with the caller's constraint assembly (custom
    // weights included), leaving the calculator primed on it so
    // constraintTotals() explains exactly this score.
    ScoreCalculator::updateShadowVariables(solution);
    calculator.resetWorkingSolution(solution);
    solution.score = fromEngineScore(calculator.calculateScore());
}

}  // namespace ta

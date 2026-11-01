#include "ta/solver.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

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
        = toEngineScoreLimit(config.bestScoreLimit);
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

// The domain Score of a plugin calculator, read from its three accumulators
// (levelScores(), before the BelowHard fold) rather than decoded from the
// folded engine hard: the decode would misfile a medium total of 10^9 or more
// as hard units and turns a saturated fold into nonsense. Only the soft level
// is still split into soft1 / soft2 by kMakespanUnit (exact at the default
// weights; see Score).
Score levelScoreOf(
    const optimaru::DecomposedScoreCalculator<TaskAssigningSolution> &calculator)
{
    const auto levels = calculator.levelScores();
    const std::int64_t soft = optimaru::roundScoreLevel(levels.soft, "soft");
    return {static_cast<std::int64_t>(levels.hard),
            optimaru::roundScoreLevel(levels.medium, "medium"),
            soft / kMakespanUnit,
            soft % kMakespanUnit};
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
        optimaru::SolverConfig engineConfig = buildEngineConfig(config_, solution, 1);
        if (config_.onNewBest)
        {
            // The engine reports a new best right after the working solution
            // became it, so the calculator holds exactly that solution:
            // report its levels like the final solution.score below.
            auto callback = config_.onNewBest;
            engineConfig.onNewBest
                = [callback, &calculator](std::int64_t step,
                                          const optimaru::HardSoftScore &)
            { callback(step, levelScoreOf(calculator)); };
        }
        auto engine = optimaru::SolverFactory<TaskAssigningSolution>::create(
                          std::move(engineConfig))
                          .buildSolver();
        engine.solve(solution, calculator);
    }

    // Re-derive the task shadow variables (employee, startTime) on the best
    // solution, then score it with the caller's constraint assembly (custom
    // weights included), leaving the calculator primed on it so
    // constraintTotals() explains exactly this score.
    ScoreCalculator::updateShadowVariables(solution);
    calculator.resetWorkingSolution(solution);
    calculator.calculateScore();
    solution.score = levelScoreOf(calculator);
}

}  // namespace ta

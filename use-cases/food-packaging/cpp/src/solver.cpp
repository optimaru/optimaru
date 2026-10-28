#include "fp/solver.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <utility>

#include "fp/incremental_calculator.hpp"
#include "fp/score_calculator.hpp"
#include "optimaru/api/solver/solver_factory.hpp"
#include "optimaru/config/solver/solver_config.hpp"

namespace fp
{

namespace
{

// Raw cleaning-window overlap minutes between every pair of lines (operator
// agnostic), from the current job lists. overlap[i][j] is what the
// operatorCleaningConflict constraint would charge if lines i and j shared
// an operator.
std::vector<std::vector<std::int64_t>>
lineOverlapMatrix(PackagingSchedule &schedule)
{
    ScoreCalculator::updateShadowVariables(schedule);
    const int lineCount = schedule.lineCount();
    std::vector<std::vector<std::pair<Minutes, Minutes>>> windows(lineCount);
    for (int l = 0; l < lineCount; ++l)
    {
        for (const int j : schedule.lines[l].jobs)
        {
            const Job &job = schedule.jobs[j];
            if (job.startCleaningDateTime < job.startProductionDateTime)
                windows[l].emplace_back(job.startCleaningDateTime,
                                        job.startProductionDateTime);
        }
    }
    std::vector<std::vector<std::int64_t>> overlap(
        lineCount, std::vector<std::int64_t>(lineCount, 0));
    for (int a = 0; a < lineCount; ++a)
    {
        for (int b = a + 1; b < lineCount; ++b)
        {
            std::int64_t total = 0;
            for (const auto &[aStart, aEnd] : windows[a])
            {
                for (const auto &[bStart, bEnd] : windows[b])
                {
                    const Minutes start = std::max(aStart, bStart);
                    const Minutes end = std::min(aEnd, bEnd);
                    if (start < end)
                        total += end - start;
                }
            }
            overlap[a][b] = total;
            overlap[b][a] = total;
        }
    }
    return overlap;
}

bool anySharedOperator(const PackagingSchedule &schedule)
{
    const int lineCount = schedule.lineCount();
    for (int a = 0; a < lineCount; ++a)
    {
        for (int b = a + 1; b < lineCount; ++b)
        {
            if (schedule.lines[a].operatorIndex
                == schedule.lines[b].operatorIndex)
                return true;
        }
    }
    return false;
}

}  // namespace

void assignMissingOperators(PackagingSchedule &schedule)
{
    const int operatorCount = static_cast<int>(schedule.operators.size());
    if (operatorCount == 0)
        return;
    std::vector<int> used(operatorCount, 0);
    for (const Line &line : schedule.lines)
    {
        if (line.operatorIndex >= 0)
            ++used[line.operatorIndex];
    }
    for (Line &line : schedule.lines)
    {
        if (line.operatorIndex >= 0)
            continue;
        int best = 0;
        for (int o = 1; o < operatorCount; ++o)
        {
            if (used[o] < used[best])
                best = o;
        }
        line.operatorIndex = best;
        ++used[best];
    }
}

bool improveOperatorAssignment(PackagingSchedule &schedule)
{
    const int operatorCount = static_cast<int>(schedule.operators.size());
    const int lineCount = schedule.lineCount();
    if (operatorCount == 0 || lineCount < 2)
        return false;

    const auto overlap = lineOverlapMatrix(schedule);
    // Conflict charged to line l against the other lines under `assign`.
    auto lineCost = [&](const std::vector<int> &assign, int l, int op)
    {
        std::int64_t cost = 0;
        for (int m = 0; m < lineCount; ++m)
        {
            if (m != l && assign[m] == op)
                cost += overlap[l][m];
        }
        return cost;
    };

    std::vector<int> assign(lineCount);
    for (int l = 0; l < lineCount; ++l)
        assign[l] = schedule.lines[l].operatorIndex;

    bool changedAny = false;
    for (bool improved = true; improved;)
    {
        improved = false;
        for (int l = 0; l < lineCount; ++l)
        {
            if (assign[l] < 0)
                continue;  // no operators were ever assigned to this line
            const std::int64_t current = lineCost(assign, l, assign[l]);
            int bestOp = assign[l];
            std::int64_t bestCost = current;
            for (int o = 0; o < operatorCount; ++o)
            {
                if (o == assign[l])
                    continue;
                const std::int64_t cost = lineCost(assign, l, o);
                if (cost < bestCost)
                {
                    bestCost = cost;
                    bestOp = o;
                }
            }
            if (bestOp != assign[l])
            {
                assign[l] = bestOp;
                improved = true;
                changedAny = true;
            }
        }
    }
    for (int l = 0; l < lineCount; ++l)
        schedule.lines[l].operatorIndex = assign[l];
    return changedAny;
}

void Solver::solve(PackagingSchedule &schedule)
{
    ScoreCalculator::prepare(schedule);
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }
    assignMissingOperators(schedule);

    const int movableJobs = static_cast<int>(schedule.engineToJob.size());
    if (schedule.jobs.empty() || schedule.lines.empty() || movableJobs == 0)
    {
        // Nothing searchable: score the instance as-is (Java would leave
        // the jobs unassigned too when there are no lines).
        ScoreCalculator::calculateScore(schedule);
        return;
    }

    // Auto rounds (R4): on the shared-operator bench 5 legs beat 3 (mean
    // medium -350 -> -163, 5W/1L over 6 seeds) and 2 (head-to-head 4/6) —
    // more alternations mean more operator re-optimizations and more
    // acceptor restarts within the same wall budget.
    const int rounds
        = config_.operatorRounds > 0
              ? config_.operatorRounds
              : (anySharedOperator(schedule) && !schedule.operators.empty()
                     ? 5
                     : 1);
    const auto deadline = std::chrono::steady_clock::now() + config_.spentLimit;

    for (int round = 0; round < rounds; ++round)
    {
        const auto remaining
            = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds(0))
            break;

        optimaru::SolverConfig engineConfig;
        engineConfig.randomSeed = config_.randomSeed
                                  + static_cast<std::uint64_t>(round)
                                        * static_cast<std::uint64_t>(std::max(
                                            1, config_.parallelSolverCount));
        engineConfig.environmentMode = config_.environmentMode;
        engineConfig.multiStartCount = std::max(1, config_.parallelSolverCount);
        if (config_.onNewBest)
        {
            auto callback = config_.onNewBest;
            engineConfig.onNewBest
                = [callback](std::int64_t step,
                             const optimaru::HardSoftScore &score)
            { callback(step, fromEngineScore(score)); };
        }
        engineConfig.termination.spentLimit = remaining / (rounds - round);
        engineConfig.termination.bestScoreLimit = config_.bestScoreLimit;
        engineConfig.termination.unimprovedSpentLimit
            = config_.unimprovedSpentLimit;
        engineConfig.termination.stepCountLimit = config_.stepCountLimit;
        engineConfig.termination.unimprovedStepCountLimit
            = config_.unimprovedStepCountLimit;
        engineConfig.localSearch.acceptor.lateAcceptanceSize
            = config_.lateAcceptanceSize != 0
                  ? config_.lateAcceptanceSize
                  : std::clamp<std::size_t>(
                        8 * static_cast<std::size_t>(movableJobs), 400, 3200);
        auto &moves = engineConfig.localSearch.moveSelector;
        moves.relocateMoveWeight = config_.relocateMoveWeight;
        moves.listSwapMoveWeight = config_.listSwapMoveWeight;
        moves.twoOptMoveWeight = config_.twoOptMoveWeight;
        moves.orOptMoveWeight = config_.orOptMoveWeight;
        moves.tailSwapMoveWeight = config_.tailSwapMoveWeight;
        if (config_.nearbyProbability > 0 && movableJobs >= 2)
        {
            engineConfig.localSearch.nearby.probability
                = config_.nearbyProbability;
            engineConfig.localSearch.nearby.size = config_.nearbySize;
            // Element ids are engine ids; the meter reads only immutable
            // problem facts (products, cleanup matrix, id mapping), so the
            // lambda stays valid across the engine's worker copies. Missing
            // cleanup pairs rank as "very far" instead of throwing (the
            // meter must never evaluate score semantics).
            engineConfig.localSearch.nearby.distanceMeter
                = [&schedule](int a, int b)
            {
                const Job &jobA = schedule.jobs[schedule.engineToJob[a]];
                const Job &jobB = schedule.jobs[schedule.engineToJob[b]];
                const Minutes ab
                    = schedule.cleanup(jobA.productIndex, jobB.productIndex);
                const Minutes ba
                    = schedule.cleanup(jobB.productIndex, jobA.productIndex);
                const double changeover
                    = (ab == kMissingCleanup || ba == kMissingCleanup)
                          ? 1e9
                          : static_cast<double>(ab + ba);
                const double idealGap = static_cast<double>(
                    std::llabs(jobA.idealEndTime - jobB.idealEndTime));
                return changeover * 1e6 + idealGap;
            };
        }
        auto &lns = engineConfig.localSearch.ruinRecreate;
        lns.enabled = config_.ruinRecreate && movableJobs >= 4;
        lns.unimprovedStepTrigger = config_.ruinRecreateUnimprovedStepTrigger;
        lns.minRuinFraction = config_.ruinRecreateMinRuinFraction;
        lns.maxRuinFraction = config_.ruinRecreateMaxRuinFraction;
        lns.maxRuinCount = config_.ruinRecreateMaxRuinCount;
        lns.operatorDecay = config_.ruinRecreateOperatorDecay;
        lns.rewards = config_.ruinRecreateRewards;
        lns.polishSteps = config_.ruinRecreatePolishSteps;

        // Construction heuristic (cheapest insertion over lines + the
        // unassigned pool) + late-acceptance local search with ALNS kicks;
        // each start gets its own calculator instance (they are stateful).
        auto engine = optimaru::SolverFactory<PackagingSchedule>::create(
                          std::move(engineConfig))
                          .buildSolver();
        const optimaru::HardSoftScore best = engine.solveMultiStart(
            schedule,
            []
            {
                return std::make_unique<
                    FoodPackagingIncrementalScoreCalculator>();
            });

        if (best >= config_.bestScoreLimit)
            break;
        // Keep running the remaining legs even when the operator assignment
        // reaches a fixed point: a leg still searches the job lists (fresh
        // acceptor + reseeded RNG = a restart-continuation), and breaking
        // here used to abandon the remaining wall budget — on a shared-
        // operator instance the alternation hit the fixed point after leg 1
        // and returned with 2/3 of the time limit unused (adopted, R3).
        if (round < rounds - 1)
            improveOperatorAssignment(schedule);
    }

    // Free posterior step: with the job lists fixed, a better operator
    // assignment can only reduce the conflict penalty (no-op when injective).
    improveOperatorAssignment(schedule);

    // The authoritative final score: a full recompute on the best solution
    // (also refreshes the job shadow variables for the caller).
    schedule.score = ScoreCalculator::calculateScore(schedule);
}

void Solver::solve(
    PackagingSchedule &schedule,
    optimaru::DecomposedScoreCalculator<PackagingSchedule> &calculator)
{
    ScoreCalculator::prepare(schedule);
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }
    assignMissingOperators(schedule);

    const int movableJobs = static_cast<int>(schedule.engineToJob.size());
    if (!schedule.jobs.empty() && !schedule.lines.empty() && movableJobs != 0)
    {
        // One search leg, one start: the operator-alternation rounds and the
        // multi-start workers both need calculators of their own, and a
        // caller-assembled plugin cannot be cloned. The move mix, the
        // changeover-nearby selection and the ALNS kicks are the same.
        optimaru::SolverConfig engineConfig;
        engineConfig.randomSeed = config_.randomSeed;
        engineConfig.environmentMode = config_.environmentMode;
        engineConfig.multiStartCount = 1;
        if (config_.onNewBest)
        {
            auto callback = config_.onNewBest;
            engineConfig.onNewBest
                = [callback](std::int64_t step,
                             const optimaru::HardSoftScore &score)
            { callback(step, fromEngineScore(score)); };
        }
        engineConfig.termination.spentLimit = config_.spentLimit;
        engineConfig.termination.bestScoreLimit = config_.bestScoreLimit;
        engineConfig.termination.unimprovedSpentLimit
            = config_.unimprovedSpentLimit;
        engineConfig.termination.stepCountLimit = config_.stepCountLimit;
        engineConfig.termination.unimprovedStepCountLimit
            = config_.unimprovedStepCountLimit;
        engineConfig.localSearch.acceptor.lateAcceptanceSize
            = config_.lateAcceptanceSize != 0
                  ? config_.lateAcceptanceSize
                  : std::clamp<std::size_t>(
                        8 * static_cast<std::size_t>(movableJobs), 400, 3200);
        auto &moves = engineConfig.localSearch.moveSelector;
        moves.relocateMoveWeight = config_.relocateMoveWeight;
        moves.listSwapMoveWeight = config_.listSwapMoveWeight;
        moves.twoOptMoveWeight = config_.twoOptMoveWeight;
        moves.orOptMoveWeight = config_.orOptMoveWeight;
        moves.tailSwapMoveWeight = config_.tailSwapMoveWeight;
        if (config_.nearbyProbability > 0 && movableJobs >= 2)
        {
            engineConfig.localSearch.nearby.probability
                = config_.nearbyProbability;
            engineConfig.localSearch.nearby.size = config_.nearbySize;
            engineConfig.localSearch.nearby.distanceMeter
                = [&schedule](int a, int b)
            {
                const Job &jobA = schedule.jobs[schedule.engineToJob[a]];
                const Job &jobB = schedule.jobs[schedule.engineToJob[b]];
                const Minutes ab
                    = schedule.cleanup(jobA.productIndex, jobB.productIndex);
                const Minutes ba
                    = schedule.cleanup(jobB.productIndex, jobA.productIndex);
                const double changeover
                    = (ab == kMissingCleanup || ba == kMissingCleanup)
                          ? 1e9
                          : static_cast<double>(ab + ba);
                const double idealGap = static_cast<double>(
                    std::llabs(jobA.idealEndTime - jobB.idealEndTime));
                return changeover * 1e6 + idealGap;
            };
        }
        auto &lns = engineConfig.localSearch.ruinRecreate;
        lns.enabled = config_.ruinRecreate && movableJobs >= 4;
        lns.unimprovedStepTrigger = config_.ruinRecreateUnimprovedStepTrigger;
        lns.minRuinFraction = config_.ruinRecreateMinRuinFraction;
        lns.maxRuinFraction = config_.ruinRecreateMaxRuinFraction;
        lns.maxRuinCount = config_.ruinRecreateMaxRuinCount;
        lns.operatorDecay = config_.ruinRecreateOperatorDecay;
        lns.rewards = config_.ruinRecreateRewards;
        lns.polishSteps = config_.ruinRecreatePolishSteps;

        auto engine = optimaru::SolverFactory<PackagingSchedule>::create(
                          std::move(engineConfig))
                          .buildSolver();
        engine.solve(schedule, calculator);
    }

    // Refresh the job shadow variables on the best solution (the plugin
    // components derive their own chain and never write these), then score it
    // with the caller's constraint assembly, leaving the calculator primed on
    // it so constraintTotals() explains exactly this score.
    ScoreCalculator::updateShadowVariables(schedule);
    calculator.resetWorkingSolution(schedule);
    schedule.score = fromEngineScore(calculator.calculateScore());
}

}  // namespace fp

#include "ba/solver.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ba/incremental_calculator.hpp"
#include "ba/score_calculator.hpp"
#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"
#include "optimaru/impl/solver/worker_threads.hpp"

namespace ba
{

namespace
{

// Translates a domain Config into the engine's SolverConfig: termination,
// late acceptance, the move mix, the ALNS knobs and the nearby distance
// meter. The meter closes over `plan`, so the caller must keep that solution
// alive for the whole solve.
void configureEngine(const Solver::Config &config,
                     const BedPlan &plan,
                     optimaru::SolverConfig &engineConfig)
{
    engineConfig.randomSeed = config.randomSeed;
    engineConfig.environmentMode = config.environmentMode;
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
              : std::clamp<std::size_t>(8 * plan.stays.size(), 8000, 12000);
    engineConfig.localSearch.moveSelector.changeMoveWeight
        = config.changeMoveWeight;
    engineConfig.localSearch.moveSelector.swapMoveWeight
        = config.swapMoveWeight;
    // R2 SCREEN: BASIC exhaustive descent at the budget-share rule
    // weight x valueRange = 0.2 (the small-value-range
    // regime).
    engineConfig.localSearch.descentWeight
        = 0.2 / static_cast<double>(plan.beds.size() + 1);
    auto &lns = engineConfig.localSearch.ruinRecreate;
    lns.enabled = config.ruinRecreate;
    lns.unimprovedStepTrigger = config.ruinRecreateUnimprovedStepTrigger;
    lns.minRuinFraction = config.ruinRecreateMinRuinFraction;
    lns.maxRuinFraction = config.ruinRecreateMaxRuinFraction;
    lns.maxRuinCount = config.ruinRecreateMaxRuinCount;
    lns.operatorDecay = config.ruinRecreateOperatorDecay;
    lns.rewards = config.ruinRecreateRewards;
    lns.polishSteps = config.ruinRecreatePolishSteps;

    // Nearby selection over the stays' date windows: two stays are close when
    // they occupy nearly the same nights. Such a pair can trade beds without
    // disturbing anyone else's occupancy, which is what makes a swap move
    // useful here, and the LNS related ruin then removes one temporal cluster
    // (the contended nights) instead of a scattered set.
    if (config.nearbyProbability > 0.0)
    {
        const BedPlan *planPtr = &plan;
        engineConfig.localSearch.nearby.distanceMeter = [planPtr](int a, int b)
        {
            const Stay &x = planPtr->stays[a];
            const Stay &y = planPtr->stays[b];
            return static_cast<double>(
                std::abs(x.arrivalDay - y.arrivalDay)
                + std::abs(x.departureDay - y.departureDay));
        };
        engineConfig.localSearch.nearby.size = std::max(1, config.nearbySize);
        engineConfig.localSearch.nearby.probability = config.nearbyProbability;
    }
}

// The domain Score of a plugin calculator, read from its three level
// accumulators (levelScores(), before the medium fold) rather than decoded
// from the folded engine soft, which would file a soft total of 10^9 or more
// (or the fraction of a non-integral medium total) under the wrong level.
Score levelScoreOf(
    const optimaru::DecomposedScoreCalculator<BedPlan> &calculator)
{
    const auto levels = calculator.levelScores();
    return Score{static_cast<std::int64_t>(levels.hard),
                 optimaru::roundScoreLevel(levels.medium, "medium"),
                 optimaru::roundScoreLevel(levels.soft, "soft")};
}

// One single-threaded solve on the monolithic fast path. The calculator
// instance is this solve's own; the engine resets it on the working solution
// at solve start. Its fixed default weights keep every level integral and,
// below ~4.3 million stay-nights, the soft total under 10^9 (the Score adapter
// in domain.hpp), so the engine score onNewBest decodes has exactly the levels
// of the final full recompute.
void solveSingle(const Solver::Config &config, BedPlan &plan)
{
    optimaru::SolverConfig engineConfig;
    configureEngine(config, plan, engineConfig);

    BedAllocationIncrementalScoreCalculator calculator;
    optimaru::Solver<BedPlan> engine(std::move(engineConfig));
    if (config.stopFlag)
        engine.adoptTerminateEarlyFlag(config.stopFlag);
    engine.solve(plan, calculator);
}

// One single-threaded solve driven by a caller-assembled plugin calculator.
// onNewBest reports the calculator's own levels, the same way as the final
// plan.score of Solver::solve(plan, calculator): the engine announces a new
// best right after the working solution became it, so the accumulators hold
// exactly that solution at that moment.
void solveSingle(const Solver::Config &config,
                 BedPlan &plan,
                 optimaru::DecomposedScoreCalculator<BedPlan> &calculator)
{
    optimaru::SolverConfig engineConfig;
    configureEngine(config, plan, engineConfig);
    if (config.onNewBest)
    {
        auto onNewBest = config.onNewBest;
        engineConfig.onNewBest
            = [onNewBest, &calculator](std::int64_t step,
                                       const optimaru::HardSoftScore &)
        { onNewBest(step, levelScoreOf(calculator)); };
    }

    optimaru::Solver<BedPlan> engine(std::move(engineConfig));
    if (config.stopFlag)
        engine.adoptTerminateEarlyFlag(config.stopFlag);
    engine.solve(plan, calculator);
}

}  // namespace

void Solver::solve(BedPlan &plan)
{
    ScoreCalculator::prepare(plan);  // intern equipment ids / priorities
    if (plan.stays.empty())
    {
        ScoreCalculator::calculateScore(plan);
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }

    // Independent parallel starts (the school-timetabling pattern): worker i
    // solves its own copy with seed + i, its own calculator and its own engine
    // config, so the nearby distance meter reads the very plan its worker
    // solves instead of a solution shared across threads. The best final score
    // wins.
    const int workerCount = std::max(1, config_.parallelSolverCount);
    if (workerCount > 1)
    {
        std::vector<BedPlan> candidates(workerCount, plan);
        std::vector<std::exception_ptr> failures(workerCount);
        // Raised by the first worker that fails: every worker's engine checks
        // it once a step, so the others stop instead of using up their budget.
        auto stop = std::make_shared<std::atomic<bool>>(false);
        // A worker that cannot be started (std::system_error) stops and joins
        // the ones already running before the error is rethrown here.
        optimaru::runWorkerThreads(
            workerCount, *stop,
            [this, &candidates, &failures, &stop](int i)
            {
                try
                {
                    Config workerConfig = config_;
                    workerConfig.parallelSolverCount = 1;
                    workerConfig.stopFlag = stop;
                    workerConfig.randomSeed
                        += static_cast<std::uint64_t>(i);
                    if (i != 0)
                        workerConfig.onNewBest = nullptr;
                    solveSingle(workerConfig, candidates[i]);
                    ScoreCalculator::calculateScore(candidates[i]);
                }
                catch (...)
                {
                    failures[i] = std::current_exception();
                    stop->store(true);
                }
            });
        for (const std::exception_ptr &failure : failures)
        {
            if (failure)
                std::rethrow_exception(failure);
        }
        int best = 0;
        for (int i = 1; i < workerCount; ++i)
        {
            if (candidates[i].score > candidates[best].score)
                best = i;
        }
        plan = std::move(candidates[best]);
        return;
    }

    // Construction heuristic + late-acceptance local search (+ ALNS kicks).
    solveSingle(config_, plan);

    // The authoritative final score: a full recompute on the best solution.
    ScoreCalculator::calculateScore(plan);
}

void Solver::solve(BedPlan &plan,
                   optimaru::DecomposedScoreCalculator<BedPlan> &calculator)
{
    ScoreCalculator::prepare(plan);  // intern equipment ids / priorities
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }
    if (!plan.stays.empty())
        solveSingle(config_, plan, calculator);

    // Score the final solution with the caller's constraint assembly (custom
    // weights and levels included) and leave the calculator primed on it, so
    // constraintTotals() explains exactly this score. The three levels come
    // straight from the calculator's accumulators (levelScores(), before the
    // medium fold) rather than from decoding the folded engine score, so a
    // demoted or re-weighted term lands on the level the assembly gave it.
    // onNewBest reported the scores of this solve the same way.
    calculator.resetWorkingSolution(plan);
    plan.score = levelScoreOf(calculator);
}

}  // namespace ba

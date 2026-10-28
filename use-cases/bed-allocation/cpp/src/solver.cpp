#include "ba/solver.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "ba/incremental_calculator.hpp"
#include "ba/score_calculator.hpp"
#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"

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
        = toEngineScore(config.bestScoreLimit);
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

// One single-threaded solve driven by whichever calculator the caller
// brought (the monolithic incremental one, or a plugin assembly).
void solveSingle(const Solver::Config &config,
                 BedPlan &plan,
                 optimaru::IncrementalScoreCalculator<BedPlan> &calculator)
{
    optimaru::SolverConfig engineConfig;
    configureEngine(config, plan, engineConfig);

    optimaru::Solver<BedPlan> engine(std::move(engineConfig));
    engine.solve(plan, calculator);
}

// One single-threaded solve on the monolithic fast path. The calculator
// instance is this solve's own; the engine resets it on the working solution
// at solve start.
void solveSingle(const Solver::Config &config, BedPlan &plan)
{
    BedAllocationIncrementalScoreCalculator calculator;
    solveSingle(config, plan, calculator);
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
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (int i = 0; i < workerCount; ++i)
        {
            workers.emplace_back(
                [this, i, &candidates, &failures]
                {
                    try
                    {
                        Config workerConfig = config_;
                        workerConfig.parallelSolverCount = 1;
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
                    }
                });
        }
        for (std::thread &worker : workers)
            worker.join();
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
    // weights included) and leave the calculator primed on it, so
    // constraintTotals() explains exactly this score.
    calculator.resetWorkingSolution(plan);
    plan.score = fromEngineScore(calculator.calculateScore());
}

}  // namespace ba

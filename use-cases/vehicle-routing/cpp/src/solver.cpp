#include "vrp/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"
#include "vrp/incremental_calculator.hpp"
#include "vrp/score_calculator.hpp"

namespace vrp
{

namespace
{

// Multi-start: independent workers on copies of the plan, best final score
// wins. The engine has its own solveMultiStart(), but it runs the threads with
// the built-in configuration only; going through Solver keeps every worker on
// the exact config the caller asked for.
void solveInParallel(const Solver::Config &config, VehicleRoutePlan &plan,
                     int workerCount)
{
    std::vector<VehicleRoutePlan> candidates(static_cast<std::size_t>(workerCount),
                                             plan);
    std::vector<std::exception_ptr> failures(static_cast<std::size_t>(workerCount));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(workerCount));
    for (int i = 0; i < workerCount; ++i)
    {
        workers.emplace_back(
            [&config, i, &candidates, &failures]
            {
                try
                {
                    Solver::Config workerConfig = config;
                    workerConfig.parallelSolverCount = 1;
                    workerConfig.randomSeed += static_cast<std::uint64_t>(i);
                    if (i != 0)
                        workerConfig.onNewBest = nullptr;  // only worker 0 reports
                    Solver worker(std::move(workerConfig));
                    worker.solve(candidates[static_cast<std::size_t>(i)]);
                }
                catch (...)
                {
                    failures[static_cast<std::size_t>(i)] = std::current_exception();
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

    std::size_t best = 0;
    for (std::size_t i = 1; i < candidates.size(); ++i)
    {
        if (candidates[i].score > candidates[best].score)
            best = i;
    }
    plan = std::move(candidates[best]);
}

}  // namespace

namespace
{

// Shared engine wiring for both solve() overloads: termination, late
// acceptance, the LIST move mix, the ALNS knobs and the driving-time nearby
// selection (all pure problem-fact lookups, so they apply to the plugin path
// too).
optimaru::SolverConfig buildEngineConfig(const Solver::Config &config,
                                         const VehicleRoutePlan &plan)
{
    optimaru::SolverConfig engineConfig;
    engineConfig.randomSeed = config.randomSeed;
    engineConfig.environmentMode = config.environmentMode;
    engineConfig.multiStartCount = 1;
    if (config.onNewBest)
    {
        auto callback = config.onNewBest;
        engineConfig.onNewBest
            = [callback](std::int64_t step, const optimaru::HardSoftScore &score)
        { callback(step, fromEngineScore(score)); };
    }

    engineConfig.termination.spentLimit = config.spentLimit;
    // The "never fires" sentinel is int64 max on every level, which would
    // overflow the medium*10^9 packing; leave the engine's own never-fires
    // default in place instead.
    if (config.bestScoreLimit.medium != std::numeric_limits<std::int64_t>::max())
    {
        engineConfig.termination.bestScoreLimit
            = toEngineScore(config.bestScoreLimit);
    }
    engineConfig.termination.unimprovedSpentLimit = config.unimprovedSpentLimit;
    engineConfig.termination.stepCountLimit = config.stepCountLimit;
    engineConfig.termination.unimprovedStepCountLimit
        = config.unimprovedStepCountLimit;

    // First Fit Decreasing: place the hardest visits while the routes are
    // still empty enough to accommodate them. "Hardest" is the tightest time
    // window first, with demand as the tie-break — a narrow window is what
    // actually rules placements out, and a visit inserted late into a full
    // route is the one that ends up in the unassigned pool.
    if (config.firstFitDecreasing)
    {
        const VehicleRoutePlan *facts = &plan;
        engineConfig.constructionHeuristic.entityDifficulty = [facts](int visitIndex)
        {
            const Visit &visit = facts->visits[static_cast<std::size_t>(visitIndex)];
            const double window = static_cast<double>(
                std::max<Seconds>(visit.maxEndTime - visit.minStartTime, 1));
            return static_cast<double>(visit.demand) + 1e6 / window;
        };
    }

    auto &acceptor = engineConfig.localSearch.acceptor;
    acceptor.acceptorType = config.acceptorType;
    // lateAcceptanceSize 0 = auto-scale with the instance (see Config). The
    // horizon shrinks as the instance grows: steps get slower with route
    // count/length, and what matters is the horizon in wall-time, not in
    // steps. Measured on CVRPLIB Set X (10s, 6 seeds, all paired wins 6-0 vs
    // the old clamp(4*visits, 200, 1600)): n=288 wants ~200 (14.3% vs 32.5%
    // mean gap), n=491 is flat across 50-200 (6.8% vs 40.7%), n=700 wants
    // ~100 (8.3% vs 42.5%). The old grow-with-n rule pushed 300+ customer
    // instances 18-34pp off the pace at fixed budget.
    acceptor.lateAcceptanceSize
        = config.lateAcceptanceSize != 0
              ? config.lateAcceptanceSize
              : std::clamp<std::size_t>(
                    60000 / std::max<std::size_t>(plan.visits.size(), 1), 100,
                    400);
    acceptor.recordToRecordTravelStartGap = config.recordToRecordStartGap;
    acceptor.recordToRecordTravelEndGap = config.recordToRecordEndGap;

    auto &moves = engineConfig.localSearch.moveSelector;
    moves.relocateMoveWeight = config.relocateMoveWeight;
    moves.listSwapMoveWeight = config.listSwapMoveWeight;
    moves.twoOptMoveWeight = config.twoOptMoveWeight;
    moves.orOptMoveWeight = config.orOptMoveWeight;
    moves.tailSwapMoveWeight = config.tailSwapMoveWeight;
    moves.interExchangeMoveWeight = config.interExchangeMoveWeight;
    moves.interExchangeMaxSegment = config.interExchangeMaxSegment;

    auto &lns = engineConfig.localSearch.ruinRecreate;
    lns.enabled = config.ruinRecreate;
    lns.unimprovedStepTrigger = config.ruinRecreateUnimprovedStepTrigger;
    lns.minRuinFraction = config.ruinRecreateMinRuinFraction;
    lns.maxRuinFraction = config.ruinRecreateMaxRuinFraction;
    lns.maxRuinCount = config.ruinRecreateMaxRuinCount;
    lns.polishSteps = config.ruinRecreatePolishSteps;
    lns.asMoveWeight = config.ruinRecreateAsMoveWeight;
    lns.asMoveMaxRuin = config.ruinRecreateAsMoveMaxRuin;
    engineConfig.localSearch.descentWeight = config.descentWeight;
    engineConfig.localSearch.descentMaxSweeps = config.descentMaxSweeps;
    engineConfig.localSearch.descentListAdvanced = config.granularDescent;
    engineConfig.localSearch.descentListMaxSegment = config.granularDescentMaxSegment;
    engineConfig.localSearch.descentListCandidateRoutes = config.granularDescentCandidateRoutes;
    engineConfig.localSearch.routeEliminationWeight
        = config.routeEliminationWeight;
    engineConfig.localSearch.routeEliminationMaxSize
        = config.routeEliminationMaxSize;
    engineConfig.localSearch.ejectionChainWeight = config.ejectionChainWeight;
    // The virtual unassigned pool is the list after the vehicles: the chains
    // seed from it, and no repair or descent ever parks a visit in it (on a
    // tightly loaded fleet that is where a ruin's leftover would go, and the
    // kick that makes it the incumbent then stalls the search; see
    // LocalSearchConfig::unassignedList).
    engineConfig.localSearch.unassignedList
        = optimaru::SolutionDescriptor<VehicleRoutePlan>::unassignedList(plan);
    engineConfig.localSearch.ejectionChainMaxSteps = config.ejectionChainMaxSteps;
    engineConfig.localSearch.ejectionChainCandidateLists
        = config.ejectionChainCandidateLists;
    engineConfig.localSearch.iteratedLocalSearchWeight = config.iteratedLocalSearchWeight;
    engineConfig.localSearch.iteratedLocalSearchMinRuin = config.iteratedLocalSearchMinRuin;
    engineConfig.localSearch.iteratedLocalSearchMaxRuin = config.iteratedLocalSearchMaxRuin;
    engineConfig.localSearch.iteratedLocalSearchMaxSegment
        = config.iteratedLocalSearchMaxSegment;
    engineConfig.localSearch.iteratedLocalSearchMaxEvaluations
        = config.iteratedLocalSearchMaxEvaluations;
    engineConfig.localSearch.ejectionChainMaxEvaluations
        = config.ejectionChainMaxEvaluations;
    lns.selectionType = config.ruinRecreateSelectionType;
    lns.operatorDecay = config.ruinRecreateOperatorDecay;
    lns.rewards = config.ruinRecreateRewards;
    lns.selectionSegmentLength = config.ruinRecreateSelectionSegmentLength;
    lns.selectionAlpha = config.ruinRecreateSelectionAlpha;
    lns.sisrRuin = config.sisrRuin;
    lns.routeRuin = config.routeRuin;
    lns.sisrMaxStringRemovals = config.sisrMaxStringRemovals;
    lns.sisrMaxStringSize = config.sisrMaxStringSize;
    lns.regretK = config.regretK;
    lns.blinkRate = config.blinkRate;

    // Driving-time nearby neighborhoods over the visits. The meter reads a
    // snapshot of the visits' location indices plus the precomputed matrix —
    // a pure lookup, safe to share with the engine for the whole solve.
    if (config.drivingTimeNearby && plan.visits.size() > 1)
    {
        auto locationOfVisit = std::make_shared<std::vector<int>>();
        locationOfVisit->reserve(plan.visits.size());
        for (const Visit &visit : plan.visits)
            locationOfVisit->push_back(visit.locationIndex);
        auto matrix = std::make_shared<std::vector<std::vector<Seconds>>>(
            plan.drivingSeconds);
        engineConfig.localSearch.nearby.distanceMeter
            = [locationOfVisit, matrix](int a, int b)
        {
            return static_cast<double>(
                (*matrix)[static_cast<std::size_t>((*locationOfVisit)[a])]
                         [static_cast<std::size_t>((*locationOfVisit)[b])]);
        };
        engineConfig.localSearch.nearby.size
            = std::max(1, std::min(config.drivingTimeNearbySize,
                                   static_cast<int>(plan.visits.size()) - 1));
        engineConfig.localSearch.nearby.probability
            = config.drivingTimeNearbyProbability;
    }
    return engineConfig;
}

}  // namespace

void Solver::solve(VehicleRoutePlan &plan)
{
    // Start with an empty pool so the construction heuristic sees every
    // unrouted visit as uninitialized and places it by cheapest insertion.
    // The pool is one of the candidate lists, so "leave this visit
    // unassigned" is simply the placement the CH picks when every route would
    // break a hard constraint — hard dominates medium, so it only wins when
    // routing genuinely cannot work. Clearing here also makes solve() robust
    // to a caller-supplied plan whose pool disagrees with its routes.
    plan.unassignedVisitIndices.clear();

    if (plan.vehicles.empty() || plan.visits.empty())
    {
        ScoreCalculator::calculateScore(plan);
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }

    const int workerCount = std::max(1, config_.parallelSolverCount);
    if (workerCount > 1)
    {
        solveInParallel(config_, plan, workerCount);
        return;
    }

    optimaru::SolverConfig engineConfig = buildEngineConfig(config_, plan);
    VehicleRoutingIncrementalScoreCalculator calculator;
    optimaru::Solver<VehicleRoutePlan> engine(std::move(engineConfig));
    engine.solve(plan, calculator);

    // The authoritative final score: a full recompute on the best solution.
    // Also refreshes the shadow variables (vehicleIndex / arrivalTime).
    plan.score = ScoreCalculator::calculateScore(plan);
}

void Solver::solve(
    VehicleRoutePlan &plan,
    optimaru::DecomposedScoreCalculator<VehicleRoutePlan> &calculator)
{
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }
    if (!plan.visits.empty() && !plan.vehicles.empty())
    {
        optimaru::Solver<VehicleRoutePlan> engine(
            buildEngineConfig(config_, plan));
        engine.solve(plan, calculator);
    }

    // Refresh the shadow variables (vehicleIndex / arrivalTime) on the best
    // solution — the plugin components derive their own chain and never write
    // these — then score it with the caller's constraint assembly, leaving the
    // calculator primed on it so constraintTotals() explains exactly this
    // score.
    ScoreCalculator::updateShadowVariables(plan);
    calculator.resetWorkingSolution(plan);
    plan.score = fromEngineScore(calculator.calculateScore());
}

}  // namespace vrp

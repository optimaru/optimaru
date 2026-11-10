#include "vrp/solver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/worker_threads.hpp"
#include "vrp/incremental_calculator.hpp"
#include "vrp/score_calculator.hpp"

namespace vrp
{

namespace
{

// Multi-start: independent workers on copies of the plan, best final score
// wins. The engine has its own solve_multistart(), but it runs the threads with
// the built-in configuration only; going through Solver keeps every worker on
// the exact config the caller asked for.
void solveInParallel(const Solver::Config &config, VehicleRoutePlan &plan,
                     int workerCount)
{
    std::vector<VehicleRoutePlan> candidates(static_cast<std::size_t>(workerCount),
                                             plan);
    std::vector<std::exception_ptr> failures(static_cast<std::size_t>(workerCount));
    // Raised by the first worker that fails: every worker's engine checks
    // it once a step, so the others stop instead of using up their budget.
    auto stop = std::make_shared<std::atomic<bool>>(false);
    // A worker that cannot be started (std::system_error) stops and joins
    // the ones already running before the error is rethrown here.
    optimaru::run_worker_threads(
        workerCount, *stop,
        [&config, &candidates, &failures, &stop](int i)
        {
            try
            {
                Solver::Config workerConfig = config;
                workerConfig.parallelSolverCount = 1;
                workerConfig.stop_flag = stop;
                workerConfig.seed += static_cast<std::uint64_t>(i);
                if (i != 0)
                    workerConfig.on_new_best = nullptr;  // only worker 0 reports
                Solver worker(std::move(workerConfig));
                worker.solve(candidates[static_cast<std::size_t>(i)]);
            }
            catch (...)
            {
                failures[static_cast<std::size_t>(i)] = std::current_exception();
                stop->store(true);
            }
        });
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
optimaru::Settings buildEngineConfig(const Solver::Config &config,
                                         const VehicleRoutePlan &plan)
{
    optimaru::Settings engineConfig;
    engineConfig.seed = config.seed;
    engineConfig.score_checks = config.score_checks;
    engineConfig.parallel_starts = 1;
    if (config.on_new_best)
    {
        auto callback = config.on_new_best;
        engineConfig.on_new_best
            = [callback](std::int64_t step, const optimaru::Score &score)
        { callback(step, fromEngineScore(score)); };
    }

    engineConfig.limits.time_limit = config.time_limit;
    // The "never fires" sentinel is int64 max on every level, which would
    // overflow the medium*10^9 packing; leave the engine's own never-fires
    // default in place instead.
    if (config.target_score.medium != std::numeric_limits<std::int64_t>::max())
    {
        engineConfig.limits.target_score
            = toEngineScoreLimit(config.target_score);
    }
    engineConfig.limits.stall_time_limit = config.stall_time_limit;
    engineConfig.limits.step_limit = config.step_limit;
    engineConfig.limits.stall_step_limit
        = config.stall_step_limit;

    // First Fit Decreasing: place the hardest visits while the routes are
    // still empty enough to accommodate them. "Hardest" is the tightest time
    // window first, with demand as the tie-break — a narrow window is what
    // actually rules placements out, and a visit inserted late into a full
    // route is the one that ends up in the unassigned pool.
    if (config.firstFitDecreasing)
    {
        const VehicleRoutePlan *facts = &plan;
        engineConfig.construction.difficulty = [facts](int visitIndex)
        {
            const Visit &visit = facts->visits[static_cast<std::size_t>(visitIndex)];
            const double window = static_cast<double>(
                std::max<Seconds>(visit.maxEndTime - visit.minStartTime, 1));
            return static_cast<double>(visit.demand) + 1e6 / window;
        };
    }

    auto &acceptor = engineConfig.search.acceptance;
    acceptor.rule = config.rule;
    // history_length 0 = auto-scale with the instance (see Config). The
    // horizon shrinks as the instance grows: steps get slower with route
    // count/length, and what matters is the horizon in wall-time, not in
    // steps. Measured on CVRPLIB Set X (10s, 6 seeds, all paired wins 6-0 vs
    // the old clamp(4*visits, 200, 1600)): n=288 wants ~200 (14.3% vs 32.5%
    // mean gap), n=491 is flat across 50-200 (6.8% vs 40.7%), n=700 wants
    // ~100 (8.3% vs 42.5%). The old grow-with-n rule pushed 300+ customer
    // instances 18-34pp off the pace at fixed budget.
    acceptor.history_length
        = config.history_length != 0
              ? config.history_length
              : std::clamp<std::size_t>(
                    60000 / std::max<std::size_t>(plan.visits.size(), 1), 100,
                    400);
    acceptor.rrt_start_gap = config.recordToRecordStartGap;
    acceptor.rrt_end_gap = config.recordToRecordEndGap;

    auto &moves = engineConfig.search.moves;
    moves.relocate_weight = config.relocate_weight;
    moves.list_swap_weight = config.list_swap_weight;
    moves.two_opt_weight = config.two_opt_weight;
    moves.or_opt_weight = config.or_opt_weight;
    moves.tail_swap_weight = config.tail_swap_weight;
    moves.cross_exchange_weight = config.cross_exchange_weight;
    moves.cross_exchange_max_run = config.cross_exchange_max_run;

    auto &lns = engineConfig.search.ruin_recreate;
    lns.enabled = config.ruin_recreate;
    lns.unimproved_step_trigger = config.ruinRecreateUnimprovedStepTrigger;
    lns.min_ruin_fraction = config.ruinRecreateMinRuinFraction;
    lns.max_ruin_fraction = config.ruinRecreateMaxRuinFraction;
    lns.max_ruin_count = config.ruinRecreateMaxRuinCount;
    lns.polish_steps = config.ruinRecreatePolishSteps;
    lns.as_move_weight = config.ruinRecreateAsMoveWeight;
    lns.as_move_max_ruin = config.ruinRecreateAsMoveMaxRuin;
    engineConfig.search.descent_weight = config.descent_weight;
    engineConfig.search.descent_max_sweeps = config.descent_max_sweeps;
    engineConfig.search.descent_list_advanced = config.granularDescent;
    engineConfig.search.descent_list_max_segment = config.granularDescentMaxSegment;
    engineConfig.search.descent_list_candidate_routes = config.granularDescentCandidateRoutes;
    engineConfig.search.route_elimination_weight
        = config.route_elimination_weight;
    engineConfig.search.route_elimination_max_size
        = config.route_elimination_max_size;
    engineConfig.search.ejection_chain_weight = config.ejection_chain_weight;
    // The virtual unassigned pool is the list after the vehicles: the chains
    // seed from it, and no repair or descent ever parks a visit in it (on a
    // tightly loaded fleet that is where a ruin's leftover would go, and the
    // kick that makes it the incumbent then stalls the search; see
    // SearchSettings::unassigned_list).
    engineConfig.search.unassigned_list
        = optimaru::SolutionTraits<VehicleRoutePlan>::unassigned_list(plan);
    engineConfig.search.ejection_chain_max_steps = config.ejection_chain_max_steps;
    engineConfig.search.ejection_chain_candidate_lists
        = config.ejection_chain_candidate_lists;
    engineConfig.search.iterated_local_search_weight = config.iterated_local_search_weight;
    engineConfig.search.iterated_local_search_min_ruin = config.iterated_local_search_min_ruin;
    engineConfig.search.iterated_local_search_max_ruin = config.iterated_local_search_max_ruin;
    engineConfig.search.iterated_local_search_max_segment
        = config.iterated_local_search_max_segment;
    engineConfig.search.iterated_local_search_max_evaluations
        = config.iterated_local_search_max_evaluations;
    engineConfig.search.ejection_chain_max_evaluations
        = config.ejection_chain_max_evaluations;
    lns.selection_type = config.ruinRecreateSelectionType;
    lns.operator_decay = config.ruinRecreateOperatorDecay;
    lns.rewards = config.ruinRecreateRewards;
    lns.selection_segment_length = config.ruinRecreateSelectionSegmentLength;
    lns.selection_alpha = config.ruinRecreateSelectionAlpha;
    lns.sisr_ruin = config.sisr_ruin;
    lns.route_ruin = config.route_ruin;
    lns.sisr_max_string_removals = config.sisr_max_string_removals;
    lns.sisr_max_string_size = config.sisr_max_string_size;
    lns.regret_k = config.regret_k;
    lns.blink_rate = config.blink_rate;

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
        engineConfig.search.nearby.distance
            = [locationOfVisit, matrix](int a, int b)
        {
            return static_cast<double>(
                (*matrix)[static_cast<std::size_t>((*locationOfVisit)[a])]
                         [static_cast<std::size_t>((*locationOfVisit)[b])]);
        };
        engineConfig.search.nearby.size
            = std::max(1, std::min(config.drivingTimeNearbySize,
                                   static_cast<int>(plan.visits.size()) - 1));
        engineConfig.search.nearby.probability
            = config.drivingTimeNearbyProbability;
    }
    return engineConfig;
}

// The plugin path's Score: the calculator's three accumulators before the
// medium fold, each level rounded on its own. Decoding the folded engine soft
// instead would file a soft total of 10^9 or more under medium.
Score levelScore(const optimaru::DecomposedObjective<VehicleRoutePlan> &calculator)
{
    const auto levels = calculator.level_scores();
    return Score{static_cast<std::int64_t>(levels.hard),
                 optimaru::round_score_level(levels.medium, "medium"),
                 optimaru::round_score_level(levels.soft, "soft")};
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
        ScoreCalculator::score(plan);
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

    optimaru::Settings engineConfig = buildEngineConfig(config_, plan);
    VehicleRoutingIncrementalScoreCalculator calculator;
    optimaru::Solver<VehicleRoutePlan> engine(std::move(engineConfig));
    if (config_.stop_flag)
        engine.share_stop_flag(config_.stop_flag);
    engine.solve(plan, calculator);

    // The authoritative final score: a full recompute on the best solution.
    // Also refreshes the derived fields (vehicleIndex / arrivalTime).
    plan.score = ScoreCalculator::score(plan);
}

void Solver::solve(
    VehicleRoutePlan &plan,
    optimaru::DecomposedObjective<VehicleRoutePlan> &calculator)
{
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }
    if (!plan.visits.empty() && !plan.vehicles.empty())
    {
        optimaru::Settings engineConfig = buildEngineConfig(config_, plan);
        if (config_.on_new_best)
        {
            // Report the same three levels as the final plan.score below. The
            // engine calls on_new_best right after the working solution became
            // the new best, so the calculator's accumulators hold exactly that
            // solution; the folded engine score it passes is not decoded.
            auto callback = config_.on_new_best;
            engineConfig.on_new_best
                = [callback, &calculator](std::int64_t step,
                                          const optimaru::Score &)
            { callback(step, levelScore(calculator)); };
        }
        optimaru::Solver<VehicleRoutePlan> engine(std::move(engineConfig));
        if (config_.stop_flag)
            engine.share_stop_flag(config_.stop_flag);
        engine.solve(plan, calculator);
    }

    // Refresh the derived fields (vehicleIndex / arrivalTime) on the best
    // solution — the plugin components derive their own chain and never write
    // these — then score it with the caller's constraint assembly, leaving the
    // calculator primed on it so constraint_totals() explains exactly this
    // score. The three levels come straight from the calculator's
    // accumulators (levelScore() above), like every on_new_best report.
    ScoreCalculator::updateShadowVariables(plan);
    calculator.rebuild(plan);
    calculator.score();
    plan.score = levelScore(calculator);
}

}  // namespace vrp

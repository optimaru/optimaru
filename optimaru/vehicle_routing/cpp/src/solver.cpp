#include "vrp/solver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "vrp/constraints.hpp"
#include "vrp/group_moves.hpp"
#include "vrp/incremental_calculator.hpp"
#include "vrp/score_calculator.hpp"
#include "vrp/visit_insertion.hpp"

namespace vrp {

// What a plan may say about groups and order (see solver.hpp). Anything else
// could never be served, and the result would be hard-infeasible without a
// word of why.
void checkVisitGroups(const VehicleRoutePlan& plan) {
    const int count = static_cast<int>(plan.visits.size());
    std::vector<int> waitingFor(plan.visits.size(), 0);
    std::vector<std::vector<int>> followers(plan.visits.size());
    for (int v = 0; v < count; ++v) {
        const Visit& visit = plan.visits[static_cast<std::size_t>(v)];
        if (visit.groupIndex < -1) {
            throw std::invalid_argument("visit " + visit.id + ": groupIndex " +
                                        std::to_string(visit.groupIndex) + " is below -1");
        }
        for (int earlier : visit.afterVisitIndices) {
            if (earlier < 0 || earlier >= count) {
                throw std::invalid_argument("visit " + visit.id + ": afterVisitIndices holds " +
                                            std::to_string(earlier) + ", out of range 0.." +
                                            std::to_string(count - 1));
            }
            const Visit& other = plan.visits[static_cast<std::size_t>(earlier)];
            if (earlier == v) {
                throw std::invalid_argument("visit " + visit.id + " comes after itself");
            }
            if (visit.groupIndex < 0 || other.groupIndex != visit.groupIndex) {
                throw std::invalid_argument("visit " + visit.id + " comes after visit " + other.id +
                                            ", which is not in its group");
            }
            // Named twice, the pair would be counted twice by visitOrderCost.
            if (std::count(visit.afterVisitIndices.begin(), visit.afterVisitIndices.end(),
                           earlier) > 1) {
                throw std::invalid_argument("visit " + visit.id + " names visit " + other.id +
                                            " twice among the visits it comes after");
            }
            ++waitingFor[static_cast<std::size_t>(v)];
            followers[static_cast<std::size_t>(earlier)].push_back(v);
        }
    }
    // Every visit can be listed behind the ones it comes after, or there is a
    // cycle.
    std::vector<int> ready;
    for (int v = 0; v < count; ++v) {
        if (waitingFor[static_cast<std::size_t>(v)] == 0) ready.push_back(v);
    }
    int listed = 0;
    while (!ready.empty()) {
        const int v = ready.back();
        ready.pop_back();
        ++listed;
        for (int follower : followers[static_cast<std::size_t>(v)]) {
            if (--waitingFor[static_cast<std::size_t>(follower)] == 0) ready.push_back(follower);
        }
    }
    if (listed < count) {
        for (int v = 0; v < count; ++v) {
            if (waitingFor[static_cast<std::size_t>(v)] > 0) {
                throw std::invalid_argument("the order of the visits has a cycle through visit " +
                                            plan.visits[static_cast<std::size_t>(v)].id);
            }
        }
    }
}

namespace {

// Which visits travel together, worked out once for the solve: construction
// and the group operators read the same object.
std::shared_ptr<const VisitGroups> visitGroupsOf(const VehicleRoutePlan& plan) {
    checkVisitGroups(plan);
    auto groups = std::make_shared<VisitGroups>();
    groups->build(plan);
    return groups;
}

// The group operators of a solve on a constraint assembly (none for a plan
// without groups). They read the plan, never the calculator, so a caller's
// assembly keeps them.
void addGroupOperators(optimaru::Solver<VehicleRoutePlan>& engine, const Solver::Config& config,
                       const std::shared_ptr<const VisitGroups>& groups) {
    GroupMoveSettings moves;
    moves.ruinRecreate = config.group_ruin_recreate;
    moves.ruinMaxVisits = config.group_ruin_max_visits;
    for (auto& move : makeGroupMoves(moves, groups)) engine.add_move_operator(std::move(move));
}

// Shared engine wiring for both solve() overloads: what settings_from() takes
// off the Config's bases (the limits, the acceptance rule, the ALNS knobs),
// then, among others, the target score, the construction order, the
// automatic late-acceptance size, the sequence move mix and the driving-time
// nearby selection (all pure problem-fact lookups, so they apply to the plugin
// path too).
optimaru::Settings buildEngineConfig(const Solver::Config& config, const VehicleRoutePlan& plan) {
    optimaru::Settings engineConfig = optimaru::settings_from(config);
    engineConfig.parallel_starts = 1;
    if (config.on_new_best) {
        auto callback = config.on_new_best;
        engineConfig.on_new_best = [callback](std::int64_t step, const optimaru::Score& score) {
            callback(step, fromEngineScore(score));
        };
    }

    // The "never fires" sentinel is int64 max on every level, which would
    // overflow the medium*10^9 packing; leave the engine's own never-fires
    // default in place instead.
    if (config.target_score.medium != std::numeric_limits<std::int64_t>::max()) {
        engineConfig.limits.target_score = toEngineScoreLimit(config.target_score);
    }

    // First Fit Decreasing: place the hardest visits while the routes are
    // still empty enough to accommodate them. "Hardest" is the tightest time
    // window first, with demand as the tie-break — a narrow window is what
    // actually rules placements out, and a visit inserted late into a full
    // route is the one that ends up in the unassigned pool.
    if (config.first_fit_decreasing) {
        const VehicleRoutePlan* facts = &plan;
        engineConfig.construction.difficulty = [facts](int visitIndex) {
            const Visit& visit = facts->visits[static_cast<std::size_t>(visitIndex)];
            const double window = static_cast<double>(
                    std::max<Seconds>(visit.maxEndTime - visit.minStartTime, 1));
            return static_cast<double>(visit.demand) + 1e6 / window;
        };
    }

    auto& acceptor = engineConfig.search.acceptance;
    // history_length 0 = auto-scale with the instance (see Config). The
    // horizon shrinks as the instance grows: steps get slower with route
    // count/length, and what matters is the horizon in wall-time, not in
    // steps. Measured on CVRPLIB Set X (10s, 6 seeds, all paired wins 6-0 vs
    // the old clamp(4*visits, 200, 1600)): n=288 wants ~200 (14.3% vs 32.5%
    // mean gap), n=491 is flat across 50-200 (6.8% vs 40.7%), n=700 wants
    // ~100 (8.3% vs 42.5%). The old grow-with-n rule pushed 300+ customer
    // instances 18-34pp off the pace at fixed budget.
    acceptor.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(60000 / std::max<std::size_t>(plan.visits.size(), 1),
                                              100, 400);

    auto& moves = engineConfig.search.moves;
    moves.relocate_weight = config.relocate_weight;
    moves.list_swap_weight = config.list_swap_weight;
    moves.two_opt_weight = config.two_opt_weight;
    moves.or_opt_weight = config.or_opt_weight;
    moves.tail_swap_weight = config.tail_swap_weight;
    moves.cross_exchange_weight = config.cross_exchange_weight;
    moves.cross_exchange_max_run = config.cross_exchange_max_run;

    engineConfig.search.descent_weight = config.descent_weight;
    engineConfig.search.descent_max_sweeps = config.descent_max_sweeps;
    engineConfig.search.descent_list_advanced = config.granular_descent;
    engineConfig.search.descent_list_max_segment = config.granular_descent_max_segment;
    engineConfig.search.descent_list_candidate_routes = config.granular_descent_candidate_routes;
    engineConfig.search.route_elimination_weight = config.route_elimination_weight;
    engineConfig.search.route_elimination_max_size = config.route_elimination_max_size;
    engineConfig.search.ejection_chain_weight = config.ejection_chain_weight;
    // The virtual unassigned pool is the list after the vehicles: the chains
    // seed from it, and no repair or descent ever parks a visit in it (on a
    // tightly loaded fleet that is where a ruin's leftover would go, and the
    // kick that makes it the incumbent then stalls the search; see
    // SearchSettings::unassigned_list).
    engineConfig.search.unassigned_list =
            optimaru::SolutionTraits<VehicleRoutePlan>::unassigned_list(plan);
    engineConfig.search.ejection_chain_max_steps = config.ejection_chain_max_steps;
    engineConfig.search.ejection_chain_candidate_lists = config.ejection_chain_candidate_lists;
    engineConfig.search.iterated_local_search_weight = config.iterated_local_search_weight;
    engineConfig.search.iterated_local_search_min_ruin = config.iterated_local_search_min_ruin;
    engineConfig.search.iterated_local_search_max_ruin = config.iterated_local_search_max_ruin;
    engineConfig.search.iterated_local_search_max_segment =
            config.iterated_local_search_max_segment;
    engineConfig.search.iterated_local_search_max_evaluations =
            config.iterated_local_search_max_evaluations;
    engineConfig.search.ejection_chain_max_evaluations = config.ejection_chain_max_evaluations;
    // The ruin & recreate knobs that are this use case's own (settings_from
    // set the shared ones).
    auto& lns = engineConfig.search.ruin_recreate;
    lns.selection_type = config.ruin_recreate_selection_type;
    lns.selection_segment_length = config.ruin_recreate_selection_segment_length;
    lns.selection_alpha = config.ruin_recreate_selection_alpha;
    lns.sisr_ruin = config.sisr_ruin;
    lns.route_ruin = config.route_ruin;
    lns.sisr_max_string_removals = config.sisr_max_string_removals;
    lns.sisr_max_string_size = config.sisr_max_string_size;
    lns.regret_k = config.regret_k;
    lns.blink_rate = config.blink_rate;

    // Driving-time nearby neighborhoods over the visits. The meter reads a
    // snapshot of the visits' location indices plus the precomputed matrix —
    // a pure lookup, safe to share with the engine for the whole solve.
    if (config.driving_time_nearby && plan.visits.size() > 1) {
        auto locationOfVisit = std::make_shared<std::vector<int>>();
        locationOfVisit->reserve(plan.visits.size());
        for (const Visit& visit : plan.visits) locationOfVisit->push_back(visit.locationIndex);
        auto matrix = std::make_shared<std::vector<std::vector<Seconds>>>(plan.drivingSeconds);
        engineConfig.search.nearby.distance = [locationOfVisit, matrix](int a, int b) {
            return static_cast<double>((*matrix)[static_cast<std::size_t>((*locationOfVisit)[a])]
                                                [static_cast<std::size_t>((*locationOfVisit)[b])]);
        };
        engineConfig.search.nearby.size =
                std::max(1, std::min(config.driving_time_nearby_size,
                                     static_cast<int>(plan.visits.size()) - 1));
        engineConfig.search.nearby.probability = config.driving_time_nearby_probability;
    }
    return engineConfig;
}

// The plugin path's Score: the calculator's three accumulators before the
// medium fold, each level rounded on its own. Decoding the folded engine soft
// instead would file a soft total of 10^9 or more under medium.
Score levelScore(const optimaru::DecomposedObjective<VehicleRoutePlan>& calculator) {
    const auto levels = calculator.level_scores();
    return Score{static_cast<std::int64_t>(levels.hard),
                 optimaru::round_score_level(levels.medium, "medium"),
                 optimaru::round_score_level(levels.soft, "soft")};
}

}  // namespace

void Solver::solve(VehicleRoutePlan& plan) {
    // Refused before anything in the plan is touched, the pool included: a
    // group index below -1, a visit that comes after one outside its group, an
    // order with a cycle.
    checkVisitGroups(plan);
    // The four core rules do not price groups, and on them a split group
    // would end as a feasible plan. A plan with groups is scored with the
    // whole default assembly instead (single-start, like every solve on a
    // caller's assembly).
    if (plan.hasVisitGroups()) {
        plan.unassignedVisitIndices.clear();
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        solve(plan, calculator);
        return;
    }
    // No group of two visits or more from here on, so nothing below places or
    // moves one.
    // Start with an empty pool so the construction heuristic sees every
    // unrouted visit as uninitialized and places it by cheapest insertion.
    // The pool is one of the candidate lists, so "leave this visit
    // unassigned" is simply the placement the CH picks when every route would
    // break a hard constraint — hard dominates medium, so it only wins when
    // routing genuinely cannot work. Clearing here also makes solve() robust
    // to a caller-supplied plan whose pool disagrees with its routes.
    plan.unassignedVisitIndices.clear();

    if (plan.vehicles.empty() || plan.visits.empty()) {
        ScoreCalculator::score(plan);
        return;
    }
    optimaru::check_config(config_);

    // Multi-start: independent starts on copies of the plan, best final score
    // wins. The engine has its own solve_multistart(), but it runs the threads
    // with the built-in configuration only; going through Solver keeps every
    // start on the exact config the caller asked for. Each start is this whole
    // solve again, on its copy, with one start (so it takes the single-start
    // path below).
    if (config_.parallel_solver_count > 1) {
        const auto solveOne = [](const Config& config, VehicleRoutePlan& candidate, int) {
            Solver(config).solve(candidate);
        };
        optimaru::solve_parallel_starts(config_, plan, solveOne);
        return;
    }

    optimaru::Settings engineConfig = buildEngineConfig(config_, plan);
    VehicleRoutingIncrementalScoreCalculator calculator;
    optimaru::Solver<VehicleRoutePlan> engine(std::move(engineConfig));
    if (config_.stop_flag) engine.share_stop_flag(config_.stop_flag);
    engine.solve(plan, calculator);

    // The authoritative final score: a full recompute on the best solution.
    // Also refreshes the derived fields (vehicleIndex / arrivalTime).
    plan.score = ScoreCalculator::score(plan);
}

void Solver::solve(VehicleRoutePlan& plan,
                   optimaru::DecomposedObjective<VehicleRoutePlan>& calculator) {
    const std::shared_ptr<const VisitGroups> groups = visitGroupsOf(plan);
    optimaru::check_config(config_);
    if (!plan.visits.empty() && !plan.vehicles.empty()) {
        constructGroups(plan, *groups);
        optimaru::Settings engineConfig = buildEngineConfig(config_, plan);
        if (config_.on_new_best) {
            // Report the same three levels as the final plan.score below. The
            // engine calls on_new_best right after the working solution became
            // the new best, so the calculator's accumulators hold exactly that
            // solution; the folded engine score it passes is not decoded.
            auto callback = config_.on_new_best;
            engineConfig.on_new_best = [callback, &calculator](std::int64_t step,
                                                               const optimaru::Score&) {
                callback(step, levelScore(calculator));
            };
        }
        optimaru::Solver<VehicleRoutePlan> engine(std::move(engineConfig));
        if (config_.stop_flag) engine.share_stop_flag(config_.stop_flag);
        addGroupOperators(engine, config_, groups);
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

Score calculateScore(VehicleRoutePlan& plan) {
    checkVisitGroups(plan);
    if (!plan.hasVisitGroups()) return ScoreCalculator::score(plan);
    // As solve(plan) ends on a plan with groups: the derived fields, then the
    // three levels of the default assembly.
    ScoreCalculator::updateShadowVariables(plan);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.rebuild(plan);
    calculator.score();
    plan.score = levelScore(calculator);
    return plan.score;
}

}  // namespace vrp

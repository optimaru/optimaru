#ifndef VRP_SOLVER_HPP
#define VRP_SOLVER_HPP

#include <cstdint>
#include <utility>

#include "optimaru/model/editor.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/select/schemes.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/solver_config.hpp"
#include "vrp/domain.hpp"

namespace vrp {

// ---------------------------------------------------------------------------
// Solver
// ---------------------------------------------------------------------------
// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/.
//
// The engine runs, in the usual phase structure:
//
//   1. Construction heuristic — cheapest insertion of every visit.
//   2. Local search — one candidate move per step (relocate / swap / 2-opt /
//      or-opt / tail swap), judged by the configured acceptor.
//   3. Adaptive ruin & recreate (ALNS) — when the local search stagnates, an
//      adaptively selected ruin operator tears out part of the solution and a
//      regret-k repair rebuilds it.
//
// The ALNS machinery uses four
// operator-selection schemes, configurable acceptance criteria, and the ruin
// operators that matter on routing problems — SISR substring removal and
// whole-route removal alongside random / related / worst.
//
// Visit groups: the visits of one group (Visit::groupIndex) ride one vehicle
// together or all wait, in an order that respects Visit::afterVisitIndices.
// When a plan has groups, they are placed whole before the engine's
// construction runs and a group operator joins the move mix
// (vrp/group_moves.hpp), which carries a group between vehicles as one unit.
// The two rules are scored by visitGroupCost and visitOrderCost, which belong
// to the constraint assembly, so solve(plan) hands a plan with groups to the
// default assembly instead of scoring it on the four core constraints.
//
// Unassigned visits: leaving a visit out of every route is modelled with
// the plan's virtual unassigned pool (see VehicleRoutePlan). solve() fills the
// pool for you, so a plan built with visits simply left out of every route is
// accepted as-is.
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   target_score      the optimum is 0/0/0. {0, kNoLevelFloor, kNoLevelFloor}
    //                     stops at the first feasible solution: kNoLevelFloor on
    //                     the medium or the soft level lifts the floor from
    //                     both (see toEngineScoreLimit), so that solution may
    //                     still leave visits unassigned.
    //   history_length 0  clamp(60000 / visitCount, 100, 400): the horizon
    //                     SHRINKS as the instance grows, keeping it roughly
    //                     constant in wall-time as the per-step cost rises
    //                     (measured on CVRPLIB Set X; see solver.cpp)
    //   on_new_best       (steps, bestScore). bestScore has the same three
    //                     levels as the final plan.score of the same solve: on
    //                     the plugin path both are read off the calculator
    //                     before the medium fold, so a soft total of 10^9 or
    //                     more stays on soft in both.
    //   acceptance        the shared AcceptanceConfig: the rule (late
    //                     acceptance by default) and each rule's parameters
    struct Config : optimaru::SolverConfigBase<Score>,
                    optimaru::RuinRecreateConfig,
                    optimaru::AcceptanceConfig {
        // The stagnation KICK (ruin 10-30%, repair, make it the incumbent
        // unconditionally) is OFF by default: on a tightly loaded fleet the
        // kicked solution is far below the best and the search never finds
        // its way back, so the best stops moving at the first kick. CVRPLIB
        // Set X, 60s, seeds 0/1: with the kick X-n289-k60 finds its last
        // improvement at 5-17s and ends 9.3-12.3% above BKS; without it the
        // search improves until the clock runs out on every instance and
        // ends 6.4/8.7% (X-n289), 17.0/17.9% (X-n895, was 22%), with
        // X-n153/491/701 unchanged or better. Destroy-and-repair still runs
        // as an acceptor-judged move (ruin_recreate_as_move_weight below).
        Config() { ruin_recreate = false; }

        // --- construction heuristic ---------------------------------------
        // First Fit Decreasing: initialize the tightest-window, largest-demand
        // visits first. Plain First Fit (input order) with false.
        bool first_fit_decreasing = true;

        // --- move mix -----------------------------------------------------
        double relocate_weight = 0.4;
        double list_swap_weight = 0.2;
        double two_opt_weight = 0.15;
        double or_opt_weight = 0.15;
        // 2-opt* (tail swap): the classic inter-route VRP move. Worth real
        // weight here — with a per-vehicle capacity and per-visit time window,
        // exchanging two route tails is the cheapest way to rebalance load
        // between vehicles without walking through infeasible intermediates.
        double tail_swap_weight = 0.1;
        // CROSS-exchange: swap a run of visits in one route with a run in
        // another (see MoveMix).
        //
        // OFF by default. The move rebalances two routes by swapping segments
        // without requiring an over-capacity intermediate state.
        double cross_exchange_weight = 0.0;
        int cross_exchange_max_run = 3;

        // --- adaptive ruin & recreate (ALNS) ------------------------------
        // Search strategies registered as move operators from
        // optimaru/cpp/optimaru/moves/ and ruin_recreate/.
        // The weights are deliberately small: all three cost far more than one
        // sampled move (a descent sweep scans a whole list pair, the other two
        // run a repair), so they earn their keep at a few percent of steps and
        // starve the ordinary move mix above that. SearchSettings records
        // why these are configured rather than adaptively learned — it was
        // tried, and it is the selection schemes that are not ready for it.
        double ruin_recreate_as_move_weight = 0.05;
        int ruin_recreate_as_move_max_ruin = 12;
        // Granular intensification (Toth & Vigo 2003): enumerate the bounded
        // Exchange/2-opt/2-opt*/CROSS neighbourhood around a nearby route
        // pair. This is more expensive than a sampled move, so keep the
        // strategy at a measured small share of the search budget.
        double descent_weight = 0.03;
        int descent_max_sweeps = 4;
        bool granular_descent = true;
        int granular_descent_max_segment = 2;
        int granular_descent_candidate_routes = 4;
        // Empty the smallest route into the others in one move. The (1,0)
        // relocations above cannot do this on their own — the payoff is the two
        // depot legs that only disappear when the LAST visit leaves, so every
        // intermediate step is uphill. 0.10 measured best; 0.02 gives +8.6%/36
        // and the difference between them is small (see the repair note in
        // core's greedy_repair.hpp about nearby anchors during a route ruin).
        double route_elimination_weight = 0.10;
        int route_elimination_max_size = 12;
        // Ejection chain: insert an unassigned visit (or take an infeasible
        // best-soft relocation) and repair the capacity/time-window breach
        // with strictly hard-improving relocations and swaps, as one move.
        // It is what reaches the slack shifts a tightly loaded fleet needs —
        // each single swap is uphill, so late acceptance never chains them.
        // 0.05 measured on CVRPLIB Set X (60s, seeds 0-1, one instance per
        // size bucket): X-n895-k37 goes from 1-2 unassigned visits to fully
        // assigned, X-n289-k60 from 15.8/14.4% to 9.7/9.3% gap, and X-n153
        // (3.1/2.6 -> 1.5/2.5), X-n491 (5.1/4.7 -> 4.8/3.8) and X-n701
        // (5.1/4.4 -> 4.3/4.4) improve too. Measured with the stagnation kick
        // still on; the final numbers with it off are under ruin_recreate
        // above and in benchmarks/vehicle-routing/README.md.
        double ejection_chain_weight = 0.05;
        int ejection_chain_max_steps = 24;
        int ejection_chain_candidate_lists = 4;
        // Score evaluations one chain may spend before it gives up.
        std::int64_t ejection_chain_max_evaluations = 50000;

        // Iterated local search as a move: ruin a few visits, repair, then
        // descend (relocate / swap / 2-opt / 2-opt* / Or-opt / CROSS around
        // each visit's nearby neighbours) until the touched routes are locally
        // optimal, and let the acceptor judge the result. 0 disables it. It
        // runs on move previews, which every VRP path serves: O(1) from
        // prefix sums on the monolithic path when no visit waits for its
        // window, one walk of the candidate route when visits wait
        // (incremental_calculator.hpp), and a per-component re-cost of the
        // candidate routes on the plugin path (DecomposedObjective).
        //
        // Defaults measured on CVRPLIB X-n289-k60 / X-n895-k37 (10s, seeds
        // 0-1, mean gap to BKS): off 12.5%; weight 0.05 with ruin 5-15 and
        // Or-opt up to 3: 7.3%; Or-opt off (segment 1): 6.0%; nearby 25:
        // 5.4%; ruin 2-8: 4.9%. At 60s that setting reaches 2.5 / 2.6%
        // (X-n289) and 4.6 / 4.7% (X-n895), from 6.0 / 8.1% and 9.7 / 11.7%.
        double iterated_local_search_weight = 0.05;
        int iterated_local_search_min_ruin = 2;
        int iterated_local_search_max_ruin = 8;
        int iterated_local_search_max_segment = 1;
        std::int64_t iterated_local_search_max_evaluations = 5000000;

        // --- visit groups (vrp/group_moves.hpp) -----------------------------
        // groupRuinRecreate: take a group out, alone or with up to
        // group_ruin_max_visits more visits (the groups and single visits of
        // nearby stops, or other groups), and put each unit back whole at its
        // cheapest feasible place. Registered only for a plan with groups.
        //
        // 30 measured on generated instances of 90 and 250 visits with half or
        // all of the visits in groups of three (10-20 s, two instances, four
        // seeds each): against 12 it drives 2-3% less on three of four
        // classes and the same on the fourth; 60 and 100 are no better.
        bool group_ruin_recreate = true;
        int group_ruin_max_visits = 30;

        // Which adaptive scheme weights the ruin operators.
        optimaru::SelectionScheme ruin_recreate_selection_type =
                optimaru::SelectionScheme::RouletteWheel;
        std::int64_t ruin_recreate_selection_segment_length = 100;
        double ruin_recreate_selection_alpha = 0.05;

        // Ruin operators joining the always-on random / related / worst trio.
        bool sisr_ruin = true;
        bool route_ruin = true;
        int sisr_max_string_removals = 3;
        int sisr_max_string_size = 12;

        // Repair: regret-k insertion (>= 2) and SISR-style blinking.
        int regret_k = 3;
        double blink_rate = 0.01;

        // --- nearby selection ---------------------------------------------
        // Driving-time nearby neighborhoods over the visits: biases move
        // partners and repair positions toward visits that are close in
        // driving time — the natural neighborhood for a travel-time objective,
        // and what makes the related ruin and the SISR ruin meaningful.
        // The ejection chain and the iterated local search need it: with it
        // off they decline every draw and only burn their share of the steps.
        bool driving_time_nearby = true;
        // Granular neighbourhood (Toth & Vigo 2003); 50 nearest customers was
        // the initial candidate horizon for route-pair intensification.
        // 25 (from 50) with the iterated local search: half the candidates
        // per visit for the descent and the repair, which bought more than
        // the lost reach on Set X (see iterated_local_search_weight).
        int driving_time_nearby_size = 25;
        double driving_time_nearby_probability = 1.0;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates plan.vehicles[*].visitIndices and
    // plan.unassignedVisitIndices, and stores the best score found in
    // plan.score. Scored on the four core constraints, unless the plan has visit
    // groups: then on the default constraint assembly (vrp/constraints.hpp),
    // as the overload below would with addDefaultConstraints(), single-start.
    void solve(VehicleRoutePlan& plan);

    // Solves with a caller-assembled plugin calculator (see
    // vrp/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same search applies, driving-time nearby selection
    // included. Weights come from the assembly, NOT from the level
    // defaults. The final plan.score and every on_new_best score are computed
    // by THIS calculator (its three levels read off level_scores(), before the
    // medium fold) and the calculator is left primed on the final solution,
    // so constraint_totals() explains exactly that score. Single-start: the
    // caller-owned calculator cannot be cloned, so parallel_solver_count is
    // ignored.
    void solve(VehicleRoutePlan& plan, optimaru::DecomposedObjective<VehicleRoutePlan>& calculator);

private:
    Config config_;
};

// What a plan may say about groups and order: no group index below -1, a
// visit comes after visits of its own group only (each a visit of the plan,
// named once, never itself), and the order has no cycle. Throws
// std::invalid_argument otherwise. Every solve runs it, and so does whatever
// scores a plan handed in from outside (calculateScore, the bindings'
// constraint_costs), since visitOrderCost reads those indices.
void checkVisitGroups(const VehicleRoutePlan& plan);

// The score of a plan as it stands, the way solve(plan) scores it: on the
// four core constraints (ScoreCalculator::score), unless the plan has visit groups,
// which those rules do not price: then on the default constraint assembly.
// Refreshes the derived fields and stores the score in plan.score. Throws
// std::invalid_argument for the groups and orders solve() refuses.
Score calculateScore(VehicleRoutePlan& plan);

}  // namespace vrp

#endif  // VRP_SOLVER_HPP

#ifndef PDP_SOLVER_HPP
#define PDP_SOLVER_HPP

#include <cstdint>
#include <utility>

#include "pdp/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/select/schemes.hpp"
#include "optimaru/solver_config.hpp"

namespace pdp {

// ---------------------------------------------------------------------------
// Solver
// ---------------------------------------------------------------------------
// The same engine as vehicle-routing, plus the operators a pickup-and-delivery
// problem needs: ones whose unit is the request, not the stop.
//
//   * The request moves (pdp/request_moves.hpp) — requestRuinRecreate takes
//     requests out of their routes and puts each back, pickup and dropoff
//     together, at its cheapest feasible slot pair; bundleRelocate and
//     bundleExchange carry several requests between two routes in one step.
//     They are what improves a plan: every candidate they offer the acceptor
//     has its requests whole.
//
//   * PairRelocateMove (a custom MoveOperator) — repairs requests whose
//     coupling is currently broken, and moves a whole request to or from the
//     unassigned pool. The engine's element-granular moves and its
//     ruin-and-recreate kick have no notion of a pair, so they leave such
//     requests behind.
//
//   * PairRuin (a custom RuinOperator) — makes the engine's kick ruin whole
//     requests rather than individual stops, so it never leaves a pickup
//     stranded without its dropoff.
//
// Requests of one group (Delivery::groupIndex) ride one vehicle together or all
// wait. Construction and the request moves place a group as one unit; the
// operators that work one request at a time stay inside a group (the pair move
// takes a request only to where another request of its group rides). The rule is
// scored by requestGroupCost, which belongs to the constraint assembly, so
// solve(plan) hands a plan with groups to the default assembly instead of
// scoring it on the seven core rules.
//
// All join the engine's adaptive selection alongside the built-ins, so their
// weight is learned rather than fixed.
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   history_length 0  clamp(4 * nodeCount, 200, 1600) up to 60 nodes, 5
    //                     above
    //   on_new_best       reports each new best in the same levels as the
    //                     final plan.score of the solve() overload in use
    struct Config : optimaru::SolverConfigBase<Score>,
                    optimaru::RuinRecreateConfig,
                    optimaru::AcceptanceConfig {
        // Two ruin & recreate defaults differ from the shared ones: a kick
        // ruins up to 40% (30% there), and the rewards are {3, 2, 1, 0.5}
        // ({33, 9, 3, 0} there).
        Config() {
            ruin_recreate_max_ruin_fraction = 0.40;
            ruin_recreate_rewards = {3.0, 2.0, 1.0, 0.5};
        }

        // --- construction heuristic ---
        // Tightest time window / largest demand first.
        bool first_fit_decreasing = true;
        // Build the initial solution one complete request at a time with a
        // feasible cheapest insertion. The generic sequence constructor sees
        // individual nodes and strands pairs on larger instances.
        bool pair_construction_heuristic = true;

        // --- acceptance: the rule (late acceptance by default) and each
        // rule's parameters are the shared AcceptanceConfig ---

        // --- move mix ---
        // The built-in element moves are deliberately down-weighted relative
        // to vehicle-routing: on a PDP most single-element moves break a pair
        // and get rejected, so the pair move carries the search. They are not
        // zero because intra-route reordering (2-opt / or-opt) is still how a
        // route's sequence gets polished once the assignment is settled.
        double relocate_weight = 0.15;
        double list_swap_weight = 0.1;
        double two_opt_weight = 0.35;
        double or_opt_weight = 0.3;
        double tail_swap_weight = 0.1;
        double cross_exchange_weight = 0.0;
        int cross_exchange_max_run = 3;

        // Whether to register the pair move operator (on by default; turn it
        // off to measure what it is worth).
        bool pair_moves = true;
        bool pair_exchange_moves = true;
        // Exchange is valuable on large route sets but steals too many steps
        // on small instances. Set 0 to force it at every size.
        int pair_exchange_min_nodes = 300;
        // Share of pair moves spent repairing a currently-broken request
        // rather than relocating an arbitrary one.
        double pair_repair_probability = 0.7;
        // Share of pair moves that drop a request into the unassigned pool or
        // pull one back out — how the medium level gets optimized at all.
        double pair_assignment_probability = 0.1;

        // --- request-level moves (pdp/request_moves.hpp) ---
        // requestRuinRecreate: take up to request_ruin_max_requests requests
        // out and reinsert each at its cheapest feasible slot pair (in random
        // order, or by regret_k; blink_rate applies). A request of a group
        // brings its whole group, which can take the count past the setting.
        bool request_ruin_recreate = true;
        int request_ruin_max_requests = 20;
        // bundleRelocate / bundleExchange: move up to bundle_max_requests
        // requests of one route to another route together, or swap two such
        // bundles. A group counts as one request here.
        bool bundle_moves = true;
        int bundle_max_requests = 3;

        // --- adaptive ruin & recreate (ALNS) ---
        bool pair_ruin = true;
        // Search-strategy parity with vehicle-routing. These
        // remain opt-in for PDP because the generic variants manipulate
        // individual nodes; the request-level move/ruin operators are the
        // defaults.
        double ruin_recreate_as_move_weight = 0.0;
        int ruin_recreate_as_move_max_ruin = 12;
        double descent_weight = 0.0;
        int descent_max_sweeps = 4;
        double route_elimination_weight = 0.0;
        int route_elimination_max_size = 12;
        optimaru::SelectionScheme ruin_recreate_selection_type =
                optimaru::SelectionScheme::RouletteWheel;
        std::int64_t ruin_recreate_selection_segment_length = 100;
        double ruin_recreate_selection_alpha = 0.05;
        bool sisr_ruin = true;
        bool route_ruin = true;
        int sisr_max_string_removals = 3;
        int sisr_max_string_size = 12;
        int regret_k = 3;
        double blink_rate = 0.01;

        // --- nearby selection ---
        bool driving_time_nearby = true;
        int driving_time_nearby_size = 20;
        double driving_time_nearby_probability = 0.8;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates plan.vehicles[*].nodeIndices and
    // plan.unassignedNodeIndices, and stores the best score in plan.score.
    // Scored on the seven core rules, unless the plan has request groups
    // (PdpRoutePlan::hasRequestGroups): then on the default constraint assembly
    // (pdp/constraints.hpp), as the overload below would with
    // addDefaultConstraints(), single-start.
    void solve(PdpRoutePlan& plan);

    // Solves with a caller-assembled plugin calculator (see
    // pdp/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same search applies, the request-level move and ruin
    // operators included (they read the plan, not the calculator). Weights
    // come from the assembly, NOT from the defaults. The final plan.score is
    // computed by THIS calculator, its three levels read before the medium
    // fold (level_scores()), so a soft total of 10^9 or more stays soft;
    // on_new_best reports its scores the same way. The calculator is left
    // primed on the final solution, so constraint_totals() explains exactly
    // that score. Single-start: the caller-owned calculator cannot be cloned,
    // so parallel_solver_count is ignored.
    void solve(PdpRoutePlan& plan, optimaru::DecomposedObjective<PdpRoutePlan>& calculator);

private:
    Config config_;
};

// The score of a plan as it stands, the way solve(plan) scores it: on the
// seven core rules (ScoreCalculator::score), unless the plan has request
// groups, which those rules do not price: then on the default constraint
// assembly. Refreshes the derived fields and stores the score in plan.score.
Score calculateScore(PdpRoutePlan& plan);

}  // namespace pdp

#endif  // PDP_SOLVER_HPP

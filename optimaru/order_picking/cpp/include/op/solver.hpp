#ifndef OP_SOLVER_HPP
#define OP_SOLVER_HPP

#include <utility>

#include "op/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"

namespace op {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/.
// (following the school-timetabling Solver pattern,
// sequence-shape edition). On top of the built-in list move mix (relocate / swap /
// 2-opt / or-opt / tail swap) and the engine's adaptive ruin-and-recreate
// (ALNS, on by default) it registers a warehouse-distance nearby meter, which
// biases move partners toward picks that are physically close — the natural
// neighborhood for a travel-distance objective.
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   history_length 0  clamp(4 * pickTaskCount, 200, 1600)
    //   ruin_recreate     lets the trolley assignment escape the local optima
    //                     that pure route polishing locks into
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // The kick trigger is high on purpose: the consolidation move reaches
        // a strong record within ~10^3 steps, and each kick unconditionally
        // replaces the incumbent and resets the acceptor, so an early kick
        // cadence keeps interrupting the slow travel-distance descent that
        // needs ~10^6 uninterrupted steps (measured on the 4x instance:
        // trigger 5000 stalled at the step-703 record; 50000 kept both the
        // kicks' order diversification and the full descent).
        Config() { ruin_recreate_unimproved_step_trigger = 50000; }

        // Sequence move mix. Weights are relative; 0 disables a move. Or-opt
        // (short-segment relocate) and tail swap (2-opt*) are on: both are
        // classic routing moves and this is a routing-shaped problem.
        double relocate_weight = 0.40;
        double list_swap_weight = 0.25;
        double two_opt_weight = 0.15;
        double or_opt_weight = 0.10;
        double tail_swap_weight = 0.10;

        // Warehouse-distance nearby selection: move partners are drawn from
        // the physically closest picks with this probability (the engine's
        // nearby neighborhoods; also drives the LNS related ruin). Size 24
        // measured best across the demo and the 2x/4x bench instances — the
        // wider neighborhood keeps enough cross-trolley partners for the
        // split-structure improvements that a tight 12-pick one misses.
        bool distance_nearby = true;
        double distance_nearby_probability = 0.5;
        int distance_nearby_size = 24;

        // Custom order-level move operator (adaptive selection alongside the
        // built-in list moves). The order-split penalty (1000 per extra
        // trolley) dominates the soft level, so every cross-trolley move at
        // element granularity is a near-certain reject; this operator works
        // at whole-order granularity instead — gather a split order onto one
        // trolley, and once none is split, relocate or swap whole orders
        // between trolleys. See OrderConsolidateMove in solver.cpp.
        bool order_consolidate_moves = true;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates trolleys[*].pickTaskIndices, refreshes the
    // pick tasks' derived fields and stores the best score found in
    // solution.score.
    void solve(OrderPickingSolution& solution);

    // Solves with a caller-assembled plugin calculator (see
    // op/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same search applies, distance-nearby selection
    // included. Weights come from the assembly, NOT from the
    // defaults. The final solution.score is computed by THIS calculator and
    // the calculator is left primed on the final solution, so
    // constraint_totals() explains exactly that score.
    //
    // Two knobs do not apply on this path: parallel_solver_count (the
    // caller-owned calculator cannot be cloned across workers) and
    // order_consolidate_moves (that operator reads the monolithic calculator's
    // live per-(trolley, order) volume table).
    void solve(OrderPickingSolution& solution,
               optimaru::DecomposedObjective<OrderPickingSolution>& calculator);

private:
    Config config_;
};

}  // namespace op

#endif  // OP_SOLVER_HPP

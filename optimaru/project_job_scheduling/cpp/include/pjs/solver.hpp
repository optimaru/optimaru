#ifndef PJS_SOLVER_HPP
#define PJS_SOLVER_HPP

#include <utility>

#include "pjs/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"

namespace pjs {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<ProjectJobSchedule>, with the engine's adaptive
// ruin-and-recreate (ALNS) kicks enabled by default.
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   history_length 0  clamp(16 * allocationCount, 400, 3200)
    //   ruin_recreate     without the kicks the LA acceptor can lock into a
    //                     local optimum within milliseconds on small instances
    //                     and never escape
    //   on_new_best       reports each new best in the same levels as the final
    //                     schedule.score of the solve() overload in use
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // Assignment move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Guided change moves for the composite (delay x executionMode)
        // encoding. A uniform draw over the value grid is delay-heavy: it
        // scatters delays uniformly over [0, delayRangeSize) although good
        // schedules keep almost every delay tiny, so most uniform change
        // moves are wasted. With this probability a change move instead
        // keeps the delay near the current one (+- small geometric delta,
        // sometimes delay-preserving to switch only the execution mode) with
        // a fresh random mode slot; the rest of the draws stay uniform for
        // diversity. 0 disables the guidance entirely.
        double near_current_delay_probability = 0.85;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates allocations[*].modeDelay, refreshes the
    // derived dates and stores the best score found in schedule.score.
    // Pinned allocations (all SOURCE/SINK ones plus user-pinned STANDARD
    // ones) keep their assignment; a pinned allocation that arrives
    // unassigned is fixed at (first execution mode, delay 0) first.
    void solve(ProjectJobSchedule& schedule);

    // Solves with a caller-assembled plugin calculator (see
    // pjs/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same search applies, delay-guided value selection
    // included. Weights and levels come from the assembly (the components'
    // kDefaultWeight / kDefaultLevel unless the caller changed them). The
    // final schedule.score is computed by THIS calculator (its three level
    // accumulators, calculator.level_scores()); on_new_best reports its scores
    // the same way. The calculator is left primed on the final solution, so
    // constraint_totals() explains exactly that score. Single-start: the
    // caller-owned calculator cannot be cloned, so parallel_solver_count is
    // ignored.
    void solve(ProjectJobSchedule& schedule,
               optimaru::DecomposedObjective<ProjectJobSchedule>& calculator);

private:
    Config config_;
};

}  // namespace pjs

#endif  // PJS_SOLVER_HPP

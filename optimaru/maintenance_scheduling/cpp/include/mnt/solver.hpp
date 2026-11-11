#ifndef MNT_SOLVER_HPP
#define MNT_SOLVER_HPP

#include <utility>

#include "mnt/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"

namespace mnt {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<MaintenanceSchedule>, with the engine's built-in
// change/swap moves and adaptive ruin-and-recreate (ALNS) enabled (the
// school-timetabling pattern).
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   history_length 0  clamp(8 * jobCount, 400, 3200) — instances here are
    //                     small (dozens of jobs), so this normally resolves to
    //                     400. It is the first stage's length: the search
    //                     restarts with a doubled history when it stalls (the
    //                     engine's history_growth)
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // Ruin fractions and cap retuned for this domain (measured): the
        // guided value selection narrows the neighborhood, so the search
        // converges sooner and needs bigger, but absolutely bounded, kicks to
        // keep escaping.
        Config() {
            ruin_recreate_min_ruin_fraction = 0.20;
            ruin_recreate_max_ruin_fraction = 0.60;
            ruin_recreate_max_ruin_count = 24;
        }

        // Assignment move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Destroy-and-repair as an ORDINARY acceptor-judged move, on top of
        // the stagnation kick (engine
        // RuinRecreateSettings::as_move_weight): probability that a step is a
        // small ruin + greedy best-placement repair instead of a sampled
        // change/swap; a rejected one is rolled back like any other move.
        // 0 disables (measured: no win on the demo instances at 10s), a
        // positive value is the per-step draw probability.
        double ruin_recreate_as_move_weight = 0.0;
        int ruin_recreate_as_move_max_ruin = 12;

        // Best-improvement descent over one entity pair (engine
        // SearchSettings::descent_weight): enumerate every crewDate for
        // both jobs plus their swap, apply the best improving one, repeat
        // until the pair is locally optimal. 0 disables (measured: loses
        // budget on the demo instances at 10s).
        double descent_weight = 0.0;
        int descent_max_sweeps = 4;

        // --- guided selection (see solver.cpp) ---
        // Probability that a change move draws the date half of the composite
        // value from the job's own [minStart, idealEnd] window instead of the
        // whole (date x crew) grid. 1.0 (the default, measured best) always
        // guides; 0 disables the guidance.
        double window_value_probability = 1.0;
        // Probability that a move seeds on a "hot" job — one currently inside
        // a tagConflictCost (two jobs sharing a tag on one day). 0 disables.
        double hot_job_probability = 0.5;
        // How many random crews a guided change move tries before settling
        // for a random one, looking for a crew that is free for the job's
        // whole span. 0 disables (uniform crew).
        int free_crew_candidates = 4;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates jobs[*].crewDate and stores the best score
    // found in schedule.score. Pinned jobs keep their assignment.
    void solve(MaintenanceSchedule& schedule);

    // Solves with a caller-assembled plugin calculator (see
    // mnt/constraints.hpp): pick components and weights, then pass the
    // assembly here. Weights and levels come from the assembly (each
    // component's kDefaultWeight / kDefaultLevel unless the caller changed
    // them). The final schedule.score is computed by THIS calculator and
    // the calculator is left primed on the final solution, so
    // constraint_totals() explains exactly that score.
    //
    // Three knobs do not apply on this path: parallel_solver_count (the
    // caller-owned calculator cannot be cloned across workers) and the two
    // guided-selection hooks (window_value_probability / hot_job_probability),
    // which read the monolithic calculator's live occupancy and hot-cell
    // state. The search is the engine's construction heuristic +
    // late-acceptance local search + ALNS kicks.
    void solve(MaintenanceSchedule& schedule,
               optimaru::DecomposedObjective<MaintenanceSchedule>& calculator);

private:
    Config config_;
};

}  // namespace mnt

#endif  // MNT_SOLVER_HPP

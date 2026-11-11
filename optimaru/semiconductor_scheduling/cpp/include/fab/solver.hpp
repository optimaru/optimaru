#ifndef FAB_SOLVER_HPP
#define FAB_SOLVER_HPP

#include <utility>

#include "fab/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/solver_config.hpp"

namespace fab {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/:
// it builds the search state (the default or the seeded (rank, machine)
// values), wires the incremental calculator, the guided change move and the
// priority-swap move, and delegates to optimaru::Solver.
//
// What solve() does, in order:
//   1. values: warmStart ? the seed state of `assignments` (seedState) : the
//      default machines (defaultChoices) at identity ranks. A warm start that
//      assigns every operation is complete: while every row holds those
//      values, the score is that of `assignments` AS SUPPLIED (never decoded).
//   2. the engine: simulated annealing over change moves (steered by the
//      value recommender: a machine switch or a small priority shift) and
//      priority swaps; parallel_solver_count independent starts (seed + i), the strictly
//      best wins, so the start is never lost.
//   3. the result: assignments = the seed as supplied if the best values are
//      the seed values, else their decode (batch ids included); score,
//      makespan and totals in data mode; warmStart = false.
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   time_limit 0           stops right after construction (the start is
    //                          returned as is; the zero-budget idiom of the
    //                          tests)
    //   history_length 0       (LATE_ACCEPTANCE) clamp(16 * rows, 400, 3200)
    //   parallel_solver_count  the constraints overload runs a single start
    //                          (see there)
    //   ruin_recreate          off by default (see the constructor)
    struct Config : optimaru::SolverConfigBase<Score>,
                    optimaru::RuinRecreateConfig,
                    optimaru::AcceptanceConfig {
        // Ruin-and-recreate (the engine's ALNS) is off here: a recreate
        // evaluates every value of every ruined row, each one a decode.
        //
        // Acceptor: simulated annealing, at the shared AcceptanceConfig's
        // automatic starting temperature (hard 0, soft 1% of the start's soft
        // score, cooling linearly over the spent limit). Measured on the
        // benchmark instances and the web demo, 1% beat late acceptance (which
        // stalls within a second on these plateaus) and 0.5% / 2% / 5%.
        Config() {
            ruin_recreate = false;
            rule = optimaru::AcceptanceRule::SimulatedAnnealing;
        }

        // Moves. The change move draws its value from the recommender: with
        // machine_move_probability another machine for the row (rank kept),
        // otherwise a small priority shift (machine kept, rank +- 1..8);
        // `change_weight` / `swap_weight` are the engine's assignment mix
        // (the built-in swap would trade whole values between unrelated
        // rows, so it is off). The priority swap exchanges the ranks of two
        // rows of different lots, both machines kept; custom_move_probability
        // gates how often a step may draw it.
        double change_weight = 1.0;
        double swap_weight = 0.0;
        double machine_move_probability = 0.55;
        bool priority_swap_moves = true;
        double custom_move_probability = 0.45;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place (see the class comment): fills assignments, score,
    // makespan, totalWeightedTardiness, totalCycleTime; warmStart = false.
    // If the search throws (for example the decoder giving up), the schedule
    // is back in data mode with its assignments untouched, and the error
    // propagates. Both overloads.
    void solve(SemiconductorSchedule& schedule);

    // The same search scored by a caller-assembled plugin calculator (see
    // fab/constraints.hpp). The final score is computed by THIS calculator in
    // data mode, and the calculator is left primed on the final assignments,
    // so constraint_totals() explains exactly that score; makespan and the
    // totals come from every constraint, as always. Single start: the
    // caller-owned calculator cannot be cloned, so parallel_solver_count is
    // ignored.
    void solve(SemiconductorSchedule& schedule,
               optimaru::DecomposedObjective<SemiconductorSchedule>& calculator);

private:
    // solve(schedule) after beginSearch(): the starts and endSearch().
    void searchFromBegun(SemiconductorSchedule& schedule);
    void runSingle(SemiconductorSchedule& schedule,
                   optimaru::Objective<SemiconductorSchedule>& calculator) const;

    Config config_;
};

// --- the search state, for the tests and the bindings ---------------------

// Step 1 of Solver::solve: fills rows[*].value (and the seed fields) from
// warmStart / assignments, and sets searching = true.
void beginSearch(SemiconductorSchedule& schedule);
// Step 3 of Solver::solve: assignments from the current values (the seed as
// supplied, or the decode), searching = false, warmStart = false, and the
// default data-mode score stored.
void endSearch(SemiconductorSchedule& schedule);

}  // namespace fab

#endif  // FAB_SOLVER_HPP

#ifndef FCS_SOLVER_HPP
#define FCS_SOLVER_HPP

#include <utility>

#include "fcs/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"

namespace fcs {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<FlightCrewSchedule>, with the engine's adaptive
// ruin-and-recreate (ALNS) enabled by default.
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   history_length 0       clamp(4 * assignmentCount, 400, 3200)
    //   parallel_solver_count  the engine's multi-start
    //   ruin_recreate          without the kicks the LA acceptor can lock into
    //                          a local optimum within milliseconds on small
    //                          instances and never escape
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // Assignment move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates flightAssignments[*].employee and stores the
    // best score found in schedule.score. Pinned assignments keep their
    // employee.
    void solve(FlightCrewSchedule& schedule);

    // Solves with a caller-assembled plugin calculator (see
    // fcs/constraints.hpp): pick components and weights, then pass the
    // assembly here. Weights come from the assembly, NOT from the component
    // defaults. The final schedule.score is computed by THIS calculator and
    // the calculator is left primed on the final solution, so
    // constraint_totals() explains exactly that score. Single-start: the
    // caller-owned calculator cannot be cloned, so parallel_solver_count is
    // ignored.
    void solve(FlightCrewSchedule& schedule,
               optimaru::DecomposedObjective<FlightCrewSchedule>& calculator);

private:
    Config config_;
};

}  // namespace fcs

#endif  // FCS_SOLVER_HPP

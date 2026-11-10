#ifndef TA_SOLVER_HPP
#define TA_SOLVER_HPP

#include <utility>

#include "ta/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"

namespace ta {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<TaskAssigningSolution> (sequence variable: cheapest-insertion
// construction + relocate/swap/2-opt/or-opt/tail-swap local search), with
// the engine's adaptive ruin-and-recreate (ALNS) kicks enabled by default.
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   history_length 0       clamp(8 * taskCount, 400, 3200)
    //   parallel_solver_count  the engine's multi-start
    //   ruin_recreate          without the kicks the LA acceptor can lock into
    //                          a local optimum within milliseconds on small
    //                          instances and never escape
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // Sequence move mix. Weights are relative; 0 disables a move. Or-opt
        // (segment relocate) and tail swap (2-opt*) are enabled here on top
        // of the engine defaults: both are natural task-queue moves.
        double relocate_weight = 0.4;
        double list_swap_weight = 0.3;
        double two_opt_weight = 0.1;
        double or_opt_weight = 0.1;
        double tail_swap_weight = 0.1;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates employees[*].tasks, recomputes the derived
    // task fields (employee, startTime) and stores the best score found
    // in solution.score. After construction every task is assigned; a task
    // list order is the employee's execution order.
    void solve(TaskAssigningSolution& solution);

    // Solves with a caller-assembled plugin calculator (see
    // ta/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same search applies — cheapest-insertion
    // construction, late-acceptance local search over the sequence moves and
    // ALNS kicks. Weights come from the assembly, NOT from the
    // default weights. The final solution.score is computed by THIS
    // calculator (decoded back into the score levels) and the calculator
    // is left primed on the final solution, so constraint_totals() explains
    // exactly that score. Single-start: the caller-owned calculator cannot be
    // cloned, so parallel_solver_count is ignored.
    void solve(TaskAssigningSolution& solution,
               optimaru::DecomposedObjective<TaskAssigningSolution>& calculator);

private:
    Config config_;
};

}  // namespace ta

#endif  // TA_SOLVER_HPP

#ifndef ALB_SOLVER_HPP
#define ALB_SOLVER_HPP

#include <utility>

#include "alb/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"

namespace alb {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/: it
// runs the domain construction (a first fit, see
// construction.hpp), freezes the station value range, wires up the
// incremental score calculator and the domain's own move (CloseStationMove)
// and value recommender, and delegates the search to
// optimaru::Solver<AssemblyLineBalancingProblem> (late acceptance + adaptive
// ruin-and-recreate on top of the built-in change / swap moves).
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   history_length 0       clamp(8 * task count, 400, 3200)
    //   parallel_solver_count  ignored by the constraint-assembly overload
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // Destroy-and-repair as an ordinary acceptor-judged move (engine
        // ruin_recreate.as_move_weight); one costs ~ruinCount * station count
        // evaluations. 0 disables it.
        double ruin_recreate_as_move_weight = 0.0;
        int ruin_recreate_as_move_max_ruin = 12;

        // Assignment move mix. Weights are relative; 0 disables a move. Swaps move
        // load across a station boundary without opening a station.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Guided change-move targets (engine local_search.value_hint):
        // with this probability a change move draws the task's new station
        // uniformly from its current window — [latest predecessor station,
        // earliest successor station] intersected with its own
        // [min_station, max_station] (and max_stations) — instead of
        // uniformly over all stations. 0 = off.
        double value_recommender_probability = 0.5;

        // Assignment best-improvement descent strategy weight (engine
        // local_search.descent_weight); ~2 * station count evaluations per
        // application. 0 (default) = off.
        double descent_weight = 0.0;

        // Upper gate on how often a step may draw CloseStationMove instead
        // of the ordinary mix; the adaptive selector still chooses inside the
        // gate. 1 = unrestricted (engine default).
        double custom_move_probability = 1.0;

        // CloseStationMove: empty one used station in a single move (see
        // solver.cpp). usedStationsCost is a step function no single-task
        // move can cross.
        bool close_station_moves = true;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: prepares the problem, constructs (clearing every
    // station first unless problem.warmStart), searches, writes the best
    // stations and problem.score (a full recompute), and resets
    // problem.warmStart to false. Throws std::invalid_argument on invalid
    // facts or a cyclic precedence graph.
    void solve(AssemblyLineBalancingProblem& problem);

    // Solves with a caller-assembled plugin calculator (see
    // alb/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same construction and search apply. The final
    // problem.score is computed by THIS calculator and the calculator is left
    // primed on the final solution, so constraint_totals() explains exactly
    // that score. Single-start: the caller-owned calculator cannot be cloned,
    // so parallel_solver_count is ignored.
    void solve(AssemblyLineBalancingProblem& problem,
               optimaru::DecomposedObjective<AssemblyLineBalancingProblem>& calculator);

private:
    Config config_;
};

}  // namespace alb

#endif  // ALB_SOLVER_HPP

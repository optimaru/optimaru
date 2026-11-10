#ifndef FL_SOLVER_HPP
#define FL_SOLVER_HPP

#include <utility>

#include "fl/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"

namespace fl {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<FacilityLocationProblem>, with the engine's adaptive
// ruin-and-recreate (ALNS) enabled on top of the built-in change/swap moves.
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   history_length 0  clamp(8 * consumerCount, 400, 3200), the scaling
    //                     adopted on the school-timetabling quality bench
    //   ruin_recreate     reassigning one consumer at a time cannot close a used
    //                     facility whose consumers must move together; a ruin
    //                     batch can
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // Destroy-and-repair as an ORDINARY move (the engine's
        // ruin_recreate.as_move_weight): a small ruin is repaired, scored and
        // judged by the acceptor like any other move — unlike the stagnation
        // kick, which is rare and unconditional. One as-move costs
        // ~ruinCount * facilityCount score evaluations, so its weight follows
        // the value-range budget-share rule (weight * facilityCount ~ 0.2)
        // measured on the school-timetabling bench. 0 disables it.
        double ruin_recreate_as_move_weight = 0.0;
        int ruin_recreate_as_move_max_ruin = 12;

        // Assignment move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Nearby selection (engine local_search.nearby): biases swap partners
        // toward geographically close consumers and upgrades the LNS related
        // ruin. The distance meter is the consumers' Euclidean degree
        // distance. probability 0 (default) leaves selection uniform-random.
        double nearby_probability = 0.0;
        int nearby_size = 20;

        // Guided change-move targets (engine local_search.value_hint):
        // with this probability the change move draws the consumer's new
        // facility from its `value_recommender_keep` nearest facilities instead
        // of uniformly over all of them. 0 (default) = off.
        double value_recommender_probability = 0.0;
        int value_recommender_keep = 8;

        // Assignment descent strategy (engine local_search.descent_weight):
        // best-improvement over one nearby consumer pair — all values of
        // both plus their swap — repeated until locally optimal. One
        // application costs ~2 * facilityCount evaluations, so weight by the
        // value range. 0 (default) = off.
        double descent_weight = 0.0;

        // Upper gate on how often a step may draw a caller-registered macro
        // move (CloseFacilityMove) instead of the ordinary mix; the adaptive
        // selector still chooses inside the gate. 1 = unrestricted (engine
        // default).
        double custom_move_probability = 1.0;

        // CloseFacilityMove: empty one used facility in a single move (see
        // solver.cpp). facilitySetupCost is a step function no single-consumer
        // move can cross, so without this the search converges in a fraction
        // of a second and then stops improving.
        bool close_facility_moves = true;
        // How far down a consumer's distance-ordered facility list the move
        // looks for a new home.
        int close_facility_keep = 8;
        // Probability that the emptied facility's consumers migrate together
        // to one alternative site instead of being redistributed over the
        // facilities that are already open.
        double close_facility_migrate_probability = 0.25;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates consumers[*].facility and stores the best
    // score found in problem.score.
    void solve(FacilityLocationProblem& problem);

    // Solves with a caller-assembled plugin calculator (see
    // fl/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same search applies — construction heuristic,
    // late-acceptance local search, ALNS kicks and CloseFacilityMove. Weights
    // and levels come from the assembly (the problem carries none). The final
    // problem.score is computed by THIS calculator and the calculator is left
    // primed on the final solution, so constraint_totals() explains exactly
    // that score. Single-start: the caller-owned calculator cannot be cloned,
    // so parallel_solver_count is ignored.
    void solve(FacilityLocationProblem& problem,
               optimaru::DecomposedObjective<FacilityLocationProblem>& calculator);

private:
    Config config_;
};

}  // namespace fl

#endif  // FL_SOLVER_HPP

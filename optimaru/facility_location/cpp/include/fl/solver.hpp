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
// ruin-and-recreate (ALNS) enabled on top of the built-in change/swap moves
// and the domain's two macro moves (CloseFacilityMove, SwapFacilityMove).
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   history_length 0  clamp(8 * consumerCount, 400, 3200), the scaling
    //                     adopted on the school-timetabling quality bench. It
    //                     is the first stage's length: the search restarts
    //                     with a doubled history when it stalls (the engine's
    //                     history_growth); a solve that starts from a given
    //                     assignment keeps this one length
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

        // Upper gate on how often a step may draw one of the domain's macro
        // moves (CloseFacilityMove, SwapFacilityMove) instead of the ordinary
        // mix; the adaptive selector still chooses inside the gate. 1 =
        // unrestricted (the engine's default). In a solve that the
        // construction starts, the macro moves are drawn on a tenth of this
        // from the first restart of late acceptance on (see runEngine in
        // solver.cpp, and swap_facility_moves for what that is worth).
        double custom_move_probability = 1.0;

        // CloseFacilityMove: empty one used facility in a single move (see
        // solver.cpp). facilitySetupCost is a step function no single-consumer
        // move can cross, so without this the search converges in a fraction
        // of a second and then stops improving.
        bool close_facility_moves = true;
        // How far down a consumer's distance-ordered facility list the move
        // looks for a new home. SwapFacilityMove uses the same shortlist for
        // the consumers of the facility it closes, so 0 switches both moves
        // off.
        int close_facility_keep = 8;
        // Probability that the emptied facility's consumers migrate together
        // to one alternative site instead of being redistributed over the
        // facilities that are already open.
        double close_facility_migrate_probability = 0.25;

        // SwapFacilityMove: open a closed facility and close the one that
        // serves its surroundings, in a single move (see solver.cpp). Without
        // it the search keeps the facilities it settled on: on the 200-consumer
        // comparison instance it ended with 11 of CP-SAT's 12 facilities and
        // an assignment within 0.03% of the best one for its own 12, 1.4%
        // behind on the score.
        //
        // Measured on the six instances benchmarks/facility-location had
        // before its ladder grew to 20 (small, demo, medium, medium-2, large,
        // xlarge; 2026-11-12, 30 s, three seeds, solved from the construction), as
        // the mean cost above CP-SAT's at 30 s on 8 workers. Stages are late
        // acceptance restarting with a doubled history; the share is what the
        // gate on the macro moves is multiplied by, in the first five rows
        // for the whole solve:
        //   as in 0.5.0 (no swaps, no stages)                    1.91%
        //   stages                                               1.71%
        //   stages, a share of 0.1                               0.91%
        //   swaps, a share of 0.1                                0.18%
        //   swaps, stages                                        0.51%
        //   swaps, stages, a share of 0.1                        0.04%
        //   the same, the share from the first restart on only   0.07%
        // The last row is what a solve does. With the share from the start
        // the macro moves are too rare to close what the construction opens,
        // and a short solve pays for it: after 50,000 steps the mean cost of
        // a 200-consumer instance over 8 seeds was 3.90 million, against
        // 3.22 million in 0.5.0 and 3.10 million with the share from the
        // first restart on (which is ahead of 0.5.0 at every step limit from
        // 20,000 to 2 million, also on 30 and on 1,200 consumers).
        //
        // From a given assignment (no stages, so the full gate; 300,000
        // steps, 36 seeds, the mean cost above CP-SAT's score of the
        // instance), from a rough assignment and from a solved 80% of the
        // consumers with the rest left to place, with swaps against without:
        //   30 consumers    0.00% against 0.18%; from the solved 80% no run
        //                   of either gets below 8.77% above: it takes one
        //                   facility more, and no move opens one without
        //                   closing another
        //   60 consumers    0.15% against 2.40%, 0.19% against 2.62%
        //   200 consumers   0.78% against 3.00%, 0.80% against 2.72%
        // A tenth of the gate there gives 0.50%, 1.22%, 1.00% and 0.95% on
        // the last four.
        bool swap_facility_moves = true;
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

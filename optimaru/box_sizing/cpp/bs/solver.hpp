#ifndef BS_SOLVER_HPP
#define BS_SOLVER_HPP

#include <utility>

#include "bs/model/domain.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/solver_config.hpp"

namespace bs {

// Thin domain-side wrapper around the generic engine in core/optimaru/: the
// domain construction (a greedy over the sizes, bs/construct/construction.hpp),
// then optimaru::Solver<BoxSizingProblem> with late acceptance over the
// built-in change/swap moves, the domain's CloseBoxMove, OpenBoxMove and
// SwapBoxMove (bs/search/moves/) and the engine's adaptive ruin-and-recreate
// (ALNS).
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   time_limit 0      no search: the construction (or the seed, completed
    //                     where a parcel is still -1) comes back as it is
    //   history_length 0  clamp(8 * boxCount, 100, 800): the search is over
    //                     sets of sizes, so the history follows the candidates,
    //                     not the parcels
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // Destroy-and-repair as an ORDINARY acceptor-judged move (the
        // engine's strategies.ruin_recreate_weight); one costs ~ruinCount *
        // boxCount score evaluations. 0 disables it.
        double ruin_recreate_as_move_weight = 0.0;
        int ruin_recreate_as_move_max_ruin = 12;

        // Assignment move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Upper gate on how often a step may draw one of the domain's moves
        // (CloseBoxMove, OpenBoxMove, SwapBoxMove) instead of the ordinary
        // mix; the adaptive selector still chooses inside the gate. 1 =
        // unrestricted (the engine's default).
        double custom_move_probability = 1.0;

        // The three moves on the set of sizes (bs/search/moves/). Without
        // them the search keeps the sizes the construction chose: see
        // open_set_move.hpp for why, and docs/use-cases/box-sizing for what
        // they are worth.
        bool close_box_moves = true;
        bool open_box_moves = true;
        bool swap_box_moves = true;

        // Late acceptance refills its history with a score just below its
        // best when it has stalled (bs/accept/reheating_late_acceptance.hpp);
        // false = plain late acceptance, which from the greedy construction
        // is a descent to the first set of sizes no single move improves.
        bool reheat = true;

        // No ruin & recreate kicks by default: a kick re-places the ruined
        // parcels one at a time, and each goes back to the size it had.
        Config() { ruin_recreate = false; }
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: sets parcels[*].box and problem.score, clears
    // warmStart. A plain solve (warmStart false) ignores the input boxes; a
    // warm start keeps them and completes the parcels still at -1.
    // Throws std::invalid_argument for what ScoreCalculator::prepare and
    // requireSolvable refuse.
    void solve(BoxSizingProblem& problem);

    // Solves with a caller-assembled plugin calculator (see
    // bs/constraints.hpp): the same construction and search, scored by the
    // assembly (custom weights, levels and exponents included). The
    // construction and the moves price the sizes with the assembly's weights
    // of voidVolumeCost, boxCost and boxTypeCost. The final problem.score is
    // computed by THIS calculator and the calculator is left primed on the
    // final solution, so constraint_totals() explains exactly that score.
    // Single-start: the caller-owned calculator cannot be cloned, so
    // parallel_solver_count is ignored.
    void solve(BoxSizingProblem& problem,
               optimaru::DecomposedObjective<BoxSizingProblem>& calculator);

private:
    Config config_;
};

}  // namespace bs

#endif  // BS_SOLVER_HPP

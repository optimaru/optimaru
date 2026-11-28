#ifndef CL_SOLVER_HPP
#define CL_SOLVER_HPP

#include <utility>

#include "cl/model/domain.hpp"
#include "optimaru/solution/editor.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/solver_config.hpp"

namespace cl {

// Thin domain-side wrapper around the generic engine in core/optimaru/: the
// domain construction (first fit decreasing by decode,
// cl/construct/construction.hpp), then optimaru::Solver<ContainerLoadingProblem>
// with late acceptance over the built-in sequence moves, the domain's
// EmptyContainerMove, RepackPairMove and SwitchContainerMove
// (cl/search/moves/) and the engine's adaptive ruin-and-recreate (ALNS).
//
// The search works on loading orders. Where the items stand is derived from
// them by the decoder (cl/score/packing.hpp) every time a list is costed, so
// a step costs a decode of the one or two lists it touches, from the place
// they change.
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   time_limit 0           no search: the construction (or the lists the
    //                          problem came with, completed) comes back as it is
    //   history_length 0       clamp(4 * itemCount, 200, 1600)
    //   parallel_solver_count  start i searches the same construction with
    //                          seed + i
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // A kick rebuilds what it ruined by trying every place of every list
        // for every ruined item, and here each try is a decode. So the kicks
        // are smaller and their polish shorter than the engine's defaults.
        Config() {
            ruin_recreate_max_ruin_count = 12;
            ruin_recreate_polish_steps = 1000;
        }

        // Largest items first, each into the first container that takes it
        // (cl/construct/construction.hpp). false = the engine's cheapest
        // insertion places what no list holds.
        bool first_fit_decreasing = true;

        // Sequence move mix. Weights are relative; 0 disables a move.
        // Relocate and swap change which container holds what and in which
        // order it is loaded; Or-opt moves a short run of the order at once.
        // A reversal (2-opt) rarely helps a loading order and is kept low.
        double relocate_weight = 0.45;
        double list_swap_weight = 0.35;
        double two_opt_weight = 0.05;
        double or_opt_weight = 0.15;

        // EmptyContainerMove: empty one of the least filled containers into
        // the others in a single move (cl/search/moves/empty_container.hpp).
        // usedContainerCost is a step function no single-item move can cross,
        // so without it the search rarely closes a container.
        bool empty_container_moves = true;
        // It empties one of this many least filled containers in use (drawn
        // uniformly; 1 = always the least filled).
        int empty_container_candidates = 3;

        // RepackPairMove: the items of two containers loaded again, dealt
        // out group by group and each share in the construction's order
        // (cl/search/moves/repack_pair.hpp). It gathers a group that two
        // containers share, and repacks a loading order that grew by moves.
        bool repack_moves = true;

        // SwitchContainerMove: the load of one container into empty
        // containers of another size or cost that cost no more: as it is
        // into one, or split over several opened cheapest first
        // (cl/search/moves/switch_container.hpp). Registered only when the
        // containers differ.
        bool switch_container_moves = true;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: sets containers[*].items, the derived fields of the
    // items and containers, and problem.score. The lists the problem comes
    // with are the starting point: they are kept, and only the items in no
    // list are constructed. Throws std::invalid_argument for a problem
    // ScoreCalculator::prepare refuses.
    void solve(ContainerLoadingProblem& problem);

    // Solves with a caller-assembled plugin calculator (see
    // cl/constraints.hpp): the same construction and search, scored by the
    // assembly (custom weights, levels and exponents included). The final
    // problem.score is computed by THIS calculator and the calculator is left
    // primed on the final solution, so constraint_totals() explains exactly
    // that score. Single-start: the caller-owned calculator cannot be cloned,
    // so parallel_solver_count is ignored.
    void solve(ContainerLoadingProblem& problem,
               optimaru::DecomposedObjective<ContainerLoadingProblem>& calculator);

private:
    Config config_;
};

}  // namespace cl

#endif  // CL_SOLVER_HPP

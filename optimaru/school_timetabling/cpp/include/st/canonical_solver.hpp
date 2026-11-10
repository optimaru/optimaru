#ifndef ST_CANONICAL_SOLVER_HPP
#define ST_CANONICAL_SOLVER_HPP

#include <utility>

#include "st/canonical_domain.hpp"
#include "st/objective_weights.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"

namespace st::canon {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/ for
// the canonical domain, following the st::Solver pattern. On top of the
// built-in change/swap moves and the engine's adaptive ruin-and-recreate (ALNS,
// on by default) it wires the domain guidance adopted on the track-2 bench:
//   - a value_hint that only proposes slots from the moved lesson's
//     allowedSlot table, so disallowed slots are never even suggested;
//   - an entity_hint preferring lessons involved in a hard violation
//     (the incremental calculator's hot sets — es guided-moves pattern);
//   - same-class nearby neighborhoods biasing swap partners to classmates
//     (class-conflict-neutral swaps) and driving the LNS related ruin;
//   - a SmartClassSwapMove operator (best-of-k classmate swap in the soft
//     phase; builtin-equivalent single move in the hard phase);
//   - a construction-heuristic difficulty order (block members, team-teaching
//     and tightly-restricted lessons first — First Fit Decreasing).
// parallel_solver_count > 1 runs the independent worker threads
// (each with its own calculator + operator instances, seed + i).
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   time_limit             the CP-SAT package's SOLVER_TIME_LIMIT_SECONDS
    //                          analog
    //   history_length 0       clamp(4 * lessonCount, 400, 3200): on the
    //                          track-2 bench the small instance prefers ~800
    //                          while the mid/large ones profit from 1600+, and
    //                          4x the lesson count tracks that optimum across
    //                          sizes
    //   parallel_solver_count  starts of this solver's own solve (see above),
    //                          not the engine's multi-start; ignored by the
    //                          constraint-assembly overload
    //   ruin_recreate          essential on dense instances, where plain LA
    //                          local search locks into a local optimum
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // Destroy-and-repair as an ORDINARY acceptor-judged move, on top of
        // the stagnation kick (engine
        // RuinRecreateSettings::as_move_weight). Probability that a step is a
        // small ruin + greedy repair instead of a sampled change/swap; a
        // rejected one is rolled back like any other move.
        double ruin_recreate_as_move_weight = 0.0;
        int ruin_recreate_as_move_max_ruin = 12;

        // Best-improvement descent over one nearby lesson pair (engine
        // SearchSettings::descent_weight): enumerate every allowed period
        // for both lessons plus their swap, apply the best improving one,
        // repeat until the pair is locally optimal. Always improving, so it
        // bypasses the acceptor. One descent costs ~2 * periodCount score
        // evaluations, so the weight is a fraction of a percent.
        double descent_weight = 0.0;
        int descent_max_sweeps = 4;

        // Assignment move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Violation-guided entity selection (the es guided-moves pattern):
        // with this probability the moved lesson is drawn from the lessons
        // currently involved in a hard violation (the hard hot sets in the
        // incremental calculator). Falls back to uniform selection once
        // feasible. 0 disables the hook entirely.
        double hot_lesson_probability = 0.7;

        // Smart same-class swap operator (the es SmartChangeMove pattern):
        // evaluate up to smart_class_swap_candidates random classmate swaps and
        // apply the best. Joins the adaptive move-operator roulette next to
        // the built-in change/swap mix.
        bool smart_class_swap_moves = true;
        int smart_class_swap_candidates = 8;

        // Best-of-k width of the re-assign move (partial solving only; see
        // ReassignMove). Wider than the swap operator's k on purpose: putting
        // a lesson back into a packed timetable needs a slot that collides
        // with nothing, and one random allowed slot almost never is — the
        // whole point of the operator is to look at enough of them.
        //
        // Swept on the track-3 bench (partial_M, 10s, seed 1), placed periods
        // / total objective: 8 -> 325 / 891748, 16 -> 328 / 862891,
        // 32 -> 342 / 722035, 64 -> 341 / 731519. Past ~32 each attempt costs
        // more score evaluations than the extra reach is worth.
        int reassign_candidates = 32;

        // Same-class nearby selection. Canonical instances are typically
        // fully packed per class (weekly hours == slot count), where a change
        // move almost always creates a class conflict and the productive
        // move is a swap between two lessons of the SAME class (class
        // occupancy is conflict-neutral by construction). The nearby distance
        // meter makes classmates each other's nearest neighbors, which biases
        // swap partners toward classmates and turns the engine's related ruin
        // into a same-class cluster ruin.
        bool same_class_nearby = true;
        // Probability that a swap partner is drawn from the nearby
        // (same-class) list instead of uniformly. Swept on the track-2 bench
        // instances (S/M/L x 3 seeds, 10s): 0.5 dominates 0.8/0.95 — enough
        // pull to keep dense instances feasible, enough diversity to keep the
        // small instances from over-focusing.
        double same_class_nearby_probability = 0.5;

        // The soft weights of the monolithic fast path, solve(table) only
        // (the model carries none). Default: the soft components'
        // kDefaultWeight. solve(table, calculator) ignores them — the
        // assembly's components carry their own weights.
        ObjectiveWeights weights;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place on the monolithic calculator (hard at the components'
    // kDefaultWeight, soft at config_.weights): mutates lessons[*].slot and
    // stores the best score found in table.score. Pinned lessons keep their
    // assignment.
    void solve(Timetable& table);

    // Solves with a caller-assembled plugin calculator (see
    // st/canonical_constraints.hpp): pick components and weights, then pass
    // the assembly here (the es Solver pattern). The same guided search
    // applies — value guidance from the allowedSlot tables, violation
    // guidance (entity recommender + the smart same-class swap operator)
    // from the calculator's penalized_units()/members_per_unit(). Weights come
    // from the assembly, NOT from config_.weights. The
    // final table.score is computed by THIS calculator and the calculator is
    // left primed on the final solution, so constraint_totals() explains
    // exactly that score. Single-start: the caller-owned calculator cannot
    // be cloned, so parallel_solver_count is ignored.
    void solve(Timetable& table, optimaru::DecomposedObjective<Timetable>& calculator);

private:
    Config config_;
};

}  // namespace st::canon

#endif  // ST_CANONICAL_SOLVER_HPP

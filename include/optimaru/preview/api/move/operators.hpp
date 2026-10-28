#ifndef OPTIMARU_PREVIEW_API_MOVE_OPERATORS_HPP
#define OPTIMARU_PREVIEW_API_MOVE_OPERATORS_HPP

#include <functional>
#include <random>
#include <vector>

#include "optimaru/impl/heuristic/selector/common/nearby/nearby_neighborhoods.hpp"
#include "optimaru/api/score/director/score_director.hpp"
// The plugin point only, never the ruin_recreate_selector.hpp umbrella: that
// one includes the localsearch config, which would close a cycle back here.
#include "optimaru/impl/heuristic/selector/move/generic/select/operator_selection_scheme.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Pluggable operators with a registration API:
// custom local searches via add_local_search, custom destroys as classes)
// ---------------------------------------------------------------------------

// What a custom operator gets to see: the score director (the ONLY way it may
// mutate the solution), the precomputed nearby lists (empty when no
// nearbyDistanceMeter is configured), and — for LIST solutions — the engine's
// live element index (elementList[e] / elementPosition[e] = where element e
// currently is; -1 = unassigned). The index is read-only for operators; the
// engine resynchronizes it automatically after a custom move (the director
// records which lists were touched).
template <typename Solution>
struct OperatorContext {
    ScoreDirector<Solution>& director;
    const NearbyNeighborhoods& nearby;
    const std::vector<int>& elementList;      // LIST kind only (empty for BASIC)
    const std::vector<int>& elementPosition;  // LIST kind only (empty for BASIC)
    // The solver's termination, for an operator that runs many evaluations
    // inside one move (a descent, a repair chain): the step loop only reads
    // the clock between moves, so such an operator asks this every few
    // hundred evaluations and returns early when it answers true. It reads
    // the wall clock, so it is not for the per-evaluation hot path. nullptr
    // when the engine provides no termination (tests driving operators by
    // hand).
    const std::function<bool()>* stopRequested = nullptr;
};

// A custom local-search move, registered with Solver::addMoveOperator().
// Selection among the built-in move mix and all registered operators is
// adaptive (roulette wheel, rewarded by move outcome — the same scheme as the
// LNS ruin operators).
//
// Contract:
//  - apply() performs ONE random move through ctx.director (never mutate the
//    solution directly), remembers whatever it needs to undo it, and reports
//    touched entity/element ids for the tabu acceptor. Return false
//    when no move is possible in the current state (the step is skipped).
//  - undoLast() exactly reverts the last successful apply(). It is called at
//    most once per apply, before any other call.
//  - outcome() is told how the move fared, after the acceptor has ruled and
//    before any undo. Optional: only an operator that adapts something of its
//    own needs it (the built-in destroy-repair move forwards it to the ruin
//    operator it drew, so a ruin is credited when it is applied as an ordinary
//    move and not only when it is applied as a stagnation kick).
template <typename Solution>
class MoveOperator {
public:
    virtual ~MoveOperator() = default;
    virtual const char* name() const = 0;
    virtual bool apply(OperatorContext<Solution>& ctx, std::mt19937_64& rng,
                       std::vector<int>& entities) = 0;
    virtual void undoLast(OperatorContext<Solution>& ctx) = 0;
    virtual void outcome(LnsOutcome /*outcome*/) {}
};

// A custom LNS ruin operator, registered with Solver::addRuinOperator().
// It only CHOOSES which entities/elements to ruin — the engine performs the
// actual removal and the greedy recreate, and adapts the operator's selection
// weight from the kick's outcome, exactly like the built-in random / related /
// worst ruins.
//
// Contract: return up to ruinCount distinct entity ids (BASIC) or element ids
// (LIST), all currently assigned. Must NOT mutate the solution.
template <typename Solution>
class RuinOperator {
public:
    virtual ~RuinOperator() = default;
    virtual const char* name() const = 0;
    virtual std::vector<int> selectRuinSet(OperatorContext<Solution>& ctx, int ruinCount,
                                           std::mt19937_64& rng) = 0;
};

}  // namespace optimaru

#endif  // OPTIMARU_PREVIEW_API_MOVE_OPERATORS_HPP

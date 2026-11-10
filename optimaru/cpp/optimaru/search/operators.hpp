// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_OPERATORS_HPP
#define OPTIMARU_SEARCH_OPERATORS_HPP

#include <functional>
#include <random>
#include <vector>

#include "optimaru/model/neighborhoods.hpp"
#include "optimaru/model/editor.hpp"
// The plugin point only, never the select/schemes.hpp umbrella: that
// one includes settings.hpp, which includes this header's scheme back.
#include "optimaru/select/scheme.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Pluggable operators: custom moves registered via Solver::add_move_operator(),
// custom ruins via Solver::add_ruin_operator()
// ---------------------------------------------------------------------------

// What a custom operator gets to see: the score editor (the ONLY way it may
// mutate the solution), the precomputed nearby lists (empty when no
// search.nearby.distance is configured), and — for sequence solutions — the engine's
// live element index (element_list[e] / element_position[e] = where element e
// currently is; -1 = unassigned). The index is read-only for operators; the
// engine resynchronizes it automatically after a custom move (the editor
// records which lists were touched).
template <typename Solution>
struct OperatorContext {
    Editor<Solution>& editor;
    const Neighborhoods& nearby;
    const std::vector<int>& element_list;      // sequence shape only (empty for assignment)
    const std::vector<int>& element_position;  // sequence shape only (empty for assignment)
    // The solver's stop condition, for an operator that runs many evaluations
    // inside one move (a descent, a repair chain): the step loop only reads
    // the clock between moves, so such an operator asks this every few
    // hundred evaluations and returns early when it answers true. It reads
    // the wall clock, so it is not for the per-evaluation hot path. nullptr
    // when the engine provides no stop condition (tests driving operators by
    // hand).
    const std::function<bool()>* stop_requested = nullptr;
};

// A custom local-search move, registered with Solver::add_move_operator().
// Selection among the built-in move mix and all registered operators is
// adaptive (roulette wheel, rewarded by move outcome — the same scheme as the
// LNS ruin operators).
//
// Contract:
//  - apply() performs ONE random move through ctx.editor (never mutate the
//    solution directly), remembers whatever it needs to undo it, and reports
//    touched entity/element ids for the tabu acceptor. Return false
//    when no move is possible in the current state (the step is skipped).
//  - undo_last() exactly reverts the last successful apply(). It is called at
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
    virtual void undo_last(OperatorContext<Solution>& ctx) = 0;
    virtual void outcome(MoveOutcome /*outcome*/) {}
};

// A custom LNS ruin operator, registered with Solver::add_ruin_operator().
// It only CHOOSES which entities/elements to ruin — the engine performs the
// actual removal and the greedy recreate, and adapts the operator's selection
// weight from the kick's outcome, exactly like the built-in random / related /
// worst ruins.
//
// Contract: return up to ruin_count distinct entity ids (assignment) or element ids
// (sequence), all currently assigned. Must NOT mutate the solution.
template <typename Solution>
class RuinOperator {
public:
    virtual ~RuinOperator() = default;
    virtual const char* name() const = 0;
    virtual std::vector<int> select_ruin_set(OperatorContext<Solution>& ctx, int ruin_count,
                                             std::mt19937_64& rng) = 0;
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_OPERATORS_HPP

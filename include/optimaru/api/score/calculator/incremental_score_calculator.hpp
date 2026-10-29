#ifndef OPTIMARU_API_SCORE_CALCULATOR_INCREMENTAL_SCORE_CALCULATOR_HPP
#define OPTIMARU_API_SCORE_CALCULATOR_INCREMENTAL_SCORE_CALCULATOR_HPP

#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// IncrementalScoreCalculator
// ---------------------------------------------------------------------------
// The domain-side constraint logic, notified around every planning-variable
// change so it can maintain the score incrementally (a move costs O(delta),
// not O(solution)).
//
// Protocol (driven by ScoreDirector):
//   1. resetWorkingSolution(solution)   — once, before solving; rebuild all state
//   2a. beforeVariableChanged(entity)   — BASIC kind: variable still holds the
//                                         OLD value: retract the contribution
//       <the engine writes the new value via SolutionDescriptor::setVariable>
//       afterVariableChanged(entity)    — variable holds the NEW value: insert
//   2b. beforeListChanged(list)         — LIST kind: the list still has its OLD
//                                         contents: retract its contribution
//       <the engine mutates the list via the SolutionDescriptor list ops>
//       afterListChanged(list)          — the list has its NEW contents: insert
//   3. calculateScore()                 — cheap assembly from maintained state
//
// Override the hook pair matching your solution's VariableKind; the other pair
// is never called. The calculator must treat unassigned state (variable == -1,
// or an element missing from every list) as contributing nothing: the
// construction heuristic scores partially initialized solutions.
// Correctness contract: after any sequence of hook pairs, calculateScore()
// must equal a full recompute on the same assignment (checked automatically
// under EnvironmentMode::FULL_ASSERT).
template <typename Solution>
class IncrementalScoreCalculator {
public:
    virtual ~IncrementalScoreCalculator() = default;

    virtual void resetWorkingSolution(Solution& workingSolution) = 0;
    virtual HardSoftScore calculateScore() = 0;

    // BASIC-variable hooks.
    virtual void beforeVariableChanged(int /*entityIndex*/) {}
    virtual void afterVariableChanged(int /*entityIndex*/) {}

    // Optional batching for composite BASIC moves. Per-entity hook pairs are
    // still delivered, but expensive derived-state refreshes may be deferred
    // until the outermost batch ends. Scoring happens only outside a batch.
    virtual void beginVariableChanges() {}
    virtual void endVariableChanges() {}

    // Optional move-evaluation transaction. A calculator may snapshot only
    // the touched derived state and restore it after the engine performs the
    // inverse variable writes for a rejected move.
    virtual void beginMoveEvaluation() {}
    virtual void beginMoveRollback() {}
    virtual void endMoveEvaluation(bool /*accepted*/) {}

    // LIST-variable hooks.
    virtual void beforeListChanged(int /*listIndex*/) {}
    virtual void afterListChanged(int /*listIndex*/) {}

    // --- LIST kind: move previews (optional) ---------------------------------
    // Whether the six preview hooks below answer for the current working
    // solution. A strategy that is only worth running on previews (the
    // iterated local search's descent, which would otherwise apply, score and
    // undo thousands of moves per draw) asks this once per draw. A calculator
    // answering true implements all six kinds; declining one move for a state
    // its cache cannot bound is fine, declining a whole kind is not. The
    // default serves none.
    virtual bool supportsPreviews() const { return false; }
    // Score the solution AS IF the move were applied, without applying it, from
    // whatever per-list data the calculator keeps (prefix sums, slack, ...).
    // Return true and fill `out` when the preview is exact; return false to
    // have the engine apply the move and ask calculateScore() as usual, so a
    // calculator may support previews for some moves, lists or states only.
    // The engine verifies every preview against the applied score under
    // FULL_ASSERT. Positions are those of the current solution:
    //  - segment move: take the `len` elements at [start, start+len) of
    //    fromList and insert them, in order, at `insertAt` of toList, where
    //    insertAt indexes toList AFTER the segment has been removed (it may be
    //    the same list);
    //  - swap: exchange the elements at (l1, p1) and (l2, p2), same or
    //    different lists; the same position twice is a no-op whose preview is
    //    the current score;
    //  - reverse: reverse [first, last] of one list;
    //  - tail swap: l1 keeps [0, c1) and takes l2's [c2, end), and vice versa;
    //  - exchange: replace l1's [p1, p1+len1) with l2's [p2, p2+len2) and vice
    //    versa, l1 != l2, either length may be 0.
    virtual bool previewSegmentMove(int /*fromList*/, int /*start*/, int /*len*/,
                                    int /*toList*/, int /*insertAt*/, HardSoftScore& /*out*/) {
        return false;
    }
    virtual bool previewSwap(int /*l1*/, int /*p1*/, int /*l2*/, int /*p2*/,
                             HardSoftScore& /*out*/) {
        return false;
    }
    virtual bool previewReverse(int /*list*/, int /*first*/, int /*last*/,
                                HardSoftScore& /*out*/) {
        return false;
    }
    virtual bool previewTailSwap(int /*l1*/, int /*c1*/, int /*l2*/, int /*c2*/,
                                 HardSoftScore& /*out*/) {
        return false;
    }
    virtual bool previewExchange(int /*l1*/, int /*p1*/, int /*len1*/, int /*l2*/, int /*p2*/,
                                 int /*len2*/, HardSoftScore& /*out*/) {
        return false;
    }
    //  - insert: put `element`, currently in no list (ruined), at (list, pos).
    virtual bool previewInsert(int /*list*/, int /*pos*/, int /*element*/,
                               HardSoftScore& /*out*/) {
        return false;
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_API_SCORE_CALCULATOR_INCREMENTAL_SCORE_CALCULATOR_HPP

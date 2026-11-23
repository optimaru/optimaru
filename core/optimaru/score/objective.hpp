// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SCORE_OBJECTIVE_HPP
#define OPTIMARU_SCORE_OBJECTIVE_HPP

#include "optimaru/score/score.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Objective
// ---------------------------------------------------------------------------
// The use case's constraint logic. The engine tells it about every change to
// a decision variable so it can keep the score up to date incrementally: a
// move costs O(change), not O(solution).
//
// Call sequence (driven by the Editor):
//   1. rebuild(solution)        before solving (and whenever the engine
//                               needs a clean slate): recompute everything
//   2a. assignment shape:
//       before_assign(entity)   the entity still has its OLD value: take its
//                               contribution out
//       <the engine writes the new value through SolutionTraits::assign>
//       after_assign(entity)    the entity has its NEW value: add it back
//   2b. sequence shape:
//       before_list_edit(list)  the list still has its OLD contents
//       <the engine edits the list through the SolutionTraits list hooks>
//       after_list_edit(list)   the list has its NEW contents
//   3. score()                  cheap: assembled from the maintained state
//
// Override the pair matching your solution's Shape; the other is never
// called. Unassigned state (value -1, or an element in no list) must
// contribute nothing: construction scores partially assigned solutions.
// Correctness contract: after any sequence of hook pairs, score() equals a
// full recompute on the same assignment (checked under ScoreChecks::Full).
template <typename Solution>
class Objective {
public:
    virtual ~Objective() = default;

    virtual void rebuild(Solution& solution) = 0;
    virtual Score score() = 0;

    // assignment-variable hooks.
    virtual void before_assign(int /*entity_index*/) {}
    virtual void after_assign(int /*entity_index*/) {}

    // Optional batching for composite assignment moves. Per-entity hook pairs are
    // still delivered, but expensive derived-state refreshes may be deferred
    // until the outermost batch ends. Scoring happens only outside a batch.
    virtual void begin_batch() {}
    virtual void end_batch() {}

    // Optional move-evaluation transaction. An objective may snapshot only
    // the touched derived state and restore it after the engine performs the
    // inverse variable writes for a rejected move.
    virtual void begin_trial() {}
    virtual void begin_rollback() {}
    virtual void end_trial(bool /*accepted*/) {}

    // sequence-variable hooks.
    virtual void before_list_edit(int /*list_index*/) {}
    virtual void after_list_edit(int /*list_index*/) {}

    // --- sequence shape: move previews (optional) ---------------------------------
    // Whether the six preview hooks below answer for the current working
    // solution. A strategy that is only worth running on previews (the
    // iterated local search's descent, which would otherwise apply, score and
    // undo thousands of moves per draw) asks this once per draw. An objective
    // answering true implements all six kinds; declining one move for a state
    // its cache cannot bound is fine, declining a whole kind is not. The
    // default serves none.
    virtual bool has_previews() const { return false; }
    // Score the solution AS IF the move were applied, without applying it, from
    // whatever per-list data the objective keeps (prefix sums, slack, ...).
    // Return true and fill `out` when the preview is exact; return false to
    // have the engine apply the move and ask score() as usual, so a
    // objective may support previews for some moves, lists or states only.
    // The engine verifies every preview against the applied score under
    // ScoreChecks::Full. Positions are those of the current solution:
    //  - segment move: take the `len` elements at [start, start+len) of
    //    from_list and insert them, in order, at `insert_at` of to_list, where
    //    insert_at indexes to_list AFTER the segment has been removed (it may be
    //    the same list);
    //  - swap: exchange the elements at (l1, p1) and (l2, p2), same or
    //    different lists; the same position twice is a no-op whose preview is
    //    the current score;
    //  - reverse: reverse [first, last] of one list;
    //  - tail swap: l1 keeps [0, c1) and takes l2's [c2, end), and vice versa;
    //  - exchange: replace l1's [p1, p1+len1) with l2's [p2, p2+len2) and vice
    //    versa, l1 != l2, either length may be 0.
    virtual bool preview_segment_move(int /*from_list*/, int /*start*/, int /*len*/,
                                      int /*to_list*/, int /*insert_at*/, Score& /*out*/) {
        return false;
    }
    virtual bool preview_swap(int /*l1*/, int /*p1*/, int /*l2*/, int /*p2*/, Score& /*out*/) {
        return false;
    }
    virtual bool preview_reverse(int /*list*/, int /*first*/, int /*last*/, Score& /*out*/) {
        return false;
    }
    virtual bool preview_tail_swap(int /*l1*/, int /*c1*/, int /*l2*/, int /*c2*/, Score& /*out*/) {
        return false;
    }
    virtual bool preview_exchange(int /*l1*/, int /*p1*/, int /*len1*/, int /*l2*/, int /*p2*/,
                                  int /*len2*/, Score& /*out*/) {
        return false;
    }
    //  - insert: put `element`, currently in no list (ruined), at (list, pos).
    virtual bool preview_insert(int /*list*/, int /*pos*/, int /*element*/, Score& /*out*/) {
        return false;
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_SCORE_OBJECTIVE_HPP

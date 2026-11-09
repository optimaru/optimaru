// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_LOCAL_LIST_MOVE_HPP
#define OPTIMARU_LOCAL_LIST_MOVE_HPP

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/model/element_index.hpp"
#include "optimaru/local/preview_evaluation.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// ListMove — one sequence move, in the words of the preview contract
// ---------------------------------------------------------------------------
// The five move kinds Objective can preview, with positions
// of the current solution exactly as that contract defines them (a segment's
// `insert_at` indexes the target list after the segment is out, and so on).
// Every sequence neighbourhood in the engine (the sampled move mix, Descent, the
// granular local search, the ejection chain) describes its candidates as
// ListMoves and previews, applies and undoes them through ListMoves<Solution>
// below, so the correspondence between a preview call and the hooks the
// objective sees when the move is applied is written once.
//
// `element1` / `element2` are the elements the move is about, for the entity
// tabu and the acceptor's memory (-1 = none); they play no part in applying
// the move, which reads the solution.
struct ListMove {
    enum class Kind { SEGMENT, SWAP, REVERSE, TAIL_SWAP, EXCHANGE };

    Kind kind = Kind::SEGMENT;
    int l1 = -1;
    int p1 = -1;
    int l2 = -1;
    int p2 = -1;
    int len1 = 0;
    int len2 = 0;
    int element1 = -1;
    int element2 = -1;

    // Longest segment the engine's own neighbourhoods generate (the granular
    // local search clamps its Or-opt length to it); applying a SEGMENT move
    // itself takes any length.
    static constexpr int kMaxSegment = 8;

    bool valid() const { return l1 >= 0; }

    // The `len` elements at [start, start+len) of `from`, inserted in order at
    // `insert_at` of `to`, which indexes `to` after the segment is out (the two
    // lists may be the same). A relocation is a segment of length 1.
    static ListMove segment(int from, int start, int len, int to, int insert_at,
                            int element1 = -1, int element2 = -1) {
        return {Kind::SEGMENT, from, start, to, insert_at, len, 0, element1, element2};
    }
    // Exchange the elements at (l1, p1) and (l2, p2); same or different lists.
    static ListMove swap(int l1, int p1, int l2, int p2, int element1 = -1, int element2 = -1) {
        return {Kind::SWAP, l1, p1, l2, p2, 0, 0, element1, element2};
    }
    // Reverse [first, last] of `list`.
    static ListMove reverse(int list, int first, int last, int element1 = -1,
                            int element2 = -1) {
        return {Kind::REVERSE, list, first, list, last, 0, 0, element1, element2};
    }
    // l1 keeps [0, c1) and takes l2's [c2, end), and vice versa; l1 != l2.
    static ListMove tail_swap(int l1, int c1, int l2, int c2, int element1 = -1,
                             int element2 = -1) {
        return {Kind::TAIL_SWAP, l1, c1, l2, c2, 0, 0, element1, element2};
    }
    // Replace l1's [p1, p1+len1) with l2's [p2, p2+len2) and vice versa;
    // l1 != l2, either length may be 0.
    static ListMove exchange(int l1, int p1, int len1, int l2, int p2, int len2,
                             int element1 = -1, int element2 = -1) {
        return {Kind::EXCHANGE, l1, p1, l2, p2, len1, len2, element1, element2};
    }
};

// Preview, apply and undo of a ListMove on a Solution, through an ElementIndex
// so the element -> (list, position) map stays current.
template <typename Solution>
struct ListMoves {
    using Traits = SolutionTraits<Solution>;

    // The objective's preview of `m`; false when it declines.
    static bool preview(Objective<Solution>& calc, const ListMove& m,
                        Score& out) {
        switch (m.kind) {
            case ListMove::Kind::SEGMENT:
                return calc.preview_segment_move(m.l1, m.p1, m.len1, m.l2, m.p2, out);
            case ListMove::Kind::SWAP:
                return calc.preview_swap(m.l1, m.p1, m.l2, m.p2, out);
            case ListMove::Kind::REVERSE:
                return calc.preview_reverse(m.l1, m.p1, m.p2, out);
            case ListMove::Kind::TAIL_SWAP:
                return calc.preview_tail_swap(m.l1, m.p1, m.l2, m.p2, out);
            case ListMove::Kind::EXCHANGE:
                return calc.preview_exchange(m.l1, m.p1, m.len1, m.l2, m.p2, m.len2, out);
        }
        return false;
    }

    static void apply(Editor<Solution>& editor, ElementIndex<Solution>& index,
                      const ListMove& m) {
        switch (m.kind) {
            case ListMove::Kind::SEGMENT:
                index.move_segment(editor, m.l1, m.p1, m.len1, m.l2, m.p2);
                break;
            case ListMove::Kind::SWAP:
                swap_in_place(editor, index, m);
                break;
            case ListMove::Kind::REVERSE:
                index.reverse(editor, m.l1, m.p1, m.p2);
                break;
            case ListMove::Kind::TAIL_SWAP:
                index.swap_tails(editor, m.l1, m.p1, m.l2, m.p2);
                break;
            case ListMove::Kind::EXCHANGE:
                index.exchange_segments(editor, m.l1, m.p1, m.len1, m.l2, m.p2, m.len2);
                break;
        }
    }

    // Inverse of apply(m) on the solution apply(m) produced.
    static void undo(Editor<Solution>& editor, ElementIndex<Solution>& index,
                     const ListMove& m) {
        switch (m.kind) {
            case ListMove::Kind::SEGMENT:
                // The segment sits at [p2, p2+len1) of l2; `p1` indexes l1
                // without the segment, which is what move_segment expects.
                index.move_segment(editor, m.l2, m.p2, m.len1, m.l1, m.p1);
                break;
            case ListMove::Kind::SWAP:  // self-inverse
                swap_in_place(editor, index, m);
                break;
            case ListMove::Kind::REVERSE:  // self-inverse
                index.reverse(editor, m.l1, m.p1, m.p2);
                break;
            case ListMove::Kind::TAIL_SWAP:  // self-inverse with the same cuts
                index.swap_tails(editor, m.l1, m.p1, m.l2, m.p2);
                break;
            case ListMove::Kind::EXCHANGE:  // the same exchange with the lengths swapped
                index.exchange_segments(editor, m.l1, m.p1, m.len2, m.l2, m.p2, m.len1);
                break;
        }
    }

    // Score of the solution after `m` (see evaluate_with_preview): the preview
    // when the objective serves one, else apply / score / undo. `served`,
    // when given, reports which.
    static Score evaluate(Editor<Solution>& editor,
                                  ElementIndex<Solution>& index, const ListMove& m,
                                  bool* served = nullptr) {
        return evaluate_with_preview(
                editor,
                [&](Objective<Solution>& calc, Score& out) {
                    return preview(calc, m, out);
                },
                [&] { apply(editor, index, m); }, [&] { undo(editor, index, m); }, served);
    }

private:
    static void swap_in_place(Editor<Solution>& editor, ElementIndex<Solution>& index,
                            const ListMove& m) {
        const Solution& solution = editor.solution();
        index.swap_elements(editor, m.l1, m.p1, m.l2, m.p2,
                           Traits::element_at(solution, m.l1, m.p1),
                           Traits::element_at(solution, m.l2, m.p2));
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_LOCAL_LIST_MOVE_HPP

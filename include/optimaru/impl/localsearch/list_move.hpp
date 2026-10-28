#ifndef OPTIMARU_IMPL_LOCALSEARCH_LIST_MOVE_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_LIST_MOVE_HPP

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/impl/localsearch/element_index.hpp"
#include "optimaru/impl/localsearch/preview_evaluation.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// ListMove — one LIST move, in the words of the preview contract
// ---------------------------------------------------------------------------
// The five move kinds IncrementalScoreCalculator can preview, with positions
// of the current solution exactly as that contract defines them (a segment's
// `insertAt` indexes the target list after the segment is out, and so on).
// Every LIST neighbourhood in the engine (the sampled move mix, Descent, the
// granular local search, the ejection chain) describes its candidates as
// ListMoves and previews, applies and undoes them through ListMoves<Solution>
// below, so the correspondence between a preview call and the hooks the
// calculator sees when the move is applied is written once.
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
    // `insertAt` of `to`, which indexes `to` after the segment is out (the two
    // lists may be the same). A relocation is a segment of length 1.
    static ListMove segment(int from, int start, int len, int to, int insertAt,
                            int element1 = -1, int element2 = -1) {
        return {Kind::SEGMENT, from, start, to, insertAt, len, 0, element1, element2};
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
    static ListMove tailSwap(int l1, int c1, int l2, int c2, int element1 = -1,
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
    using Descriptor = SolutionDescriptor<Solution>;

    // The calculator's preview of `m`; false when it declines.
    static bool preview(IncrementalScoreCalculator<Solution>& calc, const ListMove& m,
                        HardSoftScore& out) {
        switch (m.kind) {
            case ListMove::Kind::SEGMENT:
                return calc.previewSegmentMove(m.l1, m.p1, m.len1, m.l2, m.p2, out);
            case ListMove::Kind::SWAP:
                return calc.previewSwap(m.l1, m.p1, m.l2, m.p2, out);
            case ListMove::Kind::REVERSE:
                return calc.previewReverse(m.l1, m.p1, m.p2, out);
            case ListMove::Kind::TAIL_SWAP:
                return calc.previewTailSwap(m.l1, m.p1, m.l2, m.p2, out);
            case ListMove::Kind::EXCHANGE:
                return calc.previewExchange(m.l1, m.p1, m.len1, m.l2, m.p2, m.len2, out);
        }
        return false;
    }

    static void apply(ScoreDirector<Solution>& director, ElementIndex<Solution>& index,
                      const ListMove& m) {
        switch (m.kind) {
            case ListMove::Kind::SEGMENT:
                index.moveSegment(director, m.l1, m.p1, m.len1, m.l2, m.p2);
                break;
            case ListMove::Kind::SWAP:
                swapInPlace(director, index, m);
                break;
            case ListMove::Kind::REVERSE:
                index.reverse(director, m.l1, m.p1, m.p2);
                break;
            case ListMove::Kind::TAIL_SWAP:
                index.swapTails(director, m.l1, m.p1, m.l2, m.p2);
                break;
            case ListMove::Kind::EXCHANGE:
                index.exchangeSegments(director, m.l1, m.p1, m.len1, m.l2, m.p2, m.len2);
                break;
        }
    }

    // Inverse of apply(m) on the solution apply(m) produced.
    static void undo(ScoreDirector<Solution>& director, ElementIndex<Solution>& index,
                     const ListMove& m) {
        switch (m.kind) {
            case ListMove::Kind::SEGMENT:
                // The segment sits at [p2, p2+len1) of l2; `p1` indexes l1
                // without the segment, which is what moveSegment expects.
                index.moveSegment(director, m.l2, m.p2, m.len1, m.l1, m.p1);
                break;
            case ListMove::Kind::SWAP:  // self-inverse
                swapInPlace(director, index, m);
                break;
            case ListMove::Kind::REVERSE:  // self-inverse
                index.reverse(director, m.l1, m.p1, m.p2);
                break;
            case ListMove::Kind::TAIL_SWAP:  // self-inverse with the same cuts
                index.swapTails(director, m.l1, m.p1, m.l2, m.p2);
                break;
            case ListMove::Kind::EXCHANGE:  // the same exchange with the lengths swapped
                index.exchangeSegments(director, m.l1, m.p1, m.len2, m.l2, m.p2, m.len1);
                break;
        }
    }

    // Score of the solution after `m` (see evaluateWithPreview): the preview
    // when the calculator serves one, else apply / score / undo. `served`,
    // when given, reports which.
    static HardSoftScore evaluate(ScoreDirector<Solution>& director,
                                  ElementIndex<Solution>& index, const ListMove& m,
                                  bool* served = nullptr) {
        return evaluateWithPreview(
                director,
                [&](IncrementalScoreCalculator<Solution>& calc, HardSoftScore& out) {
                    return preview(calc, m, out);
                },
                [&] { apply(director, index, m); }, [&] { undo(director, index, m); }, served);
    }

private:
    static void swapInPlace(ScoreDirector<Solution>& director, ElementIndex<Solution>& index,
                            const ListMove& m) {
        const Solution& solution = director.workingSolution();
        index.swapElements(director, m.l1, m.p1, m.l2, m.p2,
                           Descriptor::getElement(solution, m.l1, m.p1),
                           Descriptor::getElement(solution, m.l2, m.p2));
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_LIST_MOVE_HPP

#ifndef OPTIMARU_IMPL_LOCALSEARCH_ELEMENT_INDEX_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_ELEMENT_INDEX_HPP

#include <utility>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/director/score_director.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// ElementIndex (LIST kind)
// ---------------------------------------------------------------------------
// element id -> (list, position), so "where is element e right now?" is O(1)
// instead of a scan over every list. A list of -1 means the element is
// currently unassigned, which happens only while a ruin is in flight.
//
// The engine mutates lists through this class rather than through the
// ScoreDirector directly: the director keeps the SCORE correct, this keeps the
// INDEX correct, and the two must move together. (A custom MoveOperator that
// writes straight through the director is a legitimate exception — the phase
// resynchronizes afterwards from ScoreDirector::touchedLists(). The built-in
// strategies do not take that route, because they need the index to be live
// mid-move: their own repair reads it to find nearby anchors.)
template <typename Solution>
class ElementIndex {
    using Descriptor = SolutionDescriptor<Solution>;

public:
    void build(const Solution& solution) {
        list_.assign(Descriptor::elementCount(solution), -1);
        position_.assign(Descriptor::elementCount(solution), -1);
        for (int l = 0; l < Descriptor::listCount(solution); ++l) reindex(solution, l);
    }

    void reindex(const Solution& solution, int list) {
        const int size = Descriptor::listSize(solution, list);
        for (int p = 0; p < size; ++p) {
            const int e = Descriptor::getElement(solution, list, p);
            list_[e] = list;
            position_[e] = p;
        }
    }

    int listOf(int element) const { return list_[element]; }
    int positionOf(int element) const { return position_[element]; }

    // Exposed as vectors because OperatorContext hands them to custom
    // operators as read-only spans.
    const std::vector<int>& lists() const { return list_; }
    const std::vector<int>& positions() const { return position_; }

    int removeAt(ScoreDirector<Solution>& director, int list, int position) {
        const int e = director.removeListElement(list, position);
        list_[e] = -1;
        position_[e] = -1;
        reindex(director.workingSolution(), list);
        return e;
    }

    void insertAt(ScoreDirector<Solution>& director, int list, int position, int element) {
        director.insertListElement(list, position, element);
        reindex(director.workingSolution(), list);
    }

    void reverse(ScoreDirector<Solution>& director, int list, int first, int last) {
        director.reverseListSegment(list, first, last);
        reindex(director.workingSolution(), list);
    }

    // Move the `len` elements at [start, start+len) of `from` to `insertAt`
    // of `to`, in order; `insertAt` indexes `to` after the segment is out
    // (the lists may be the same). One hook pair per element, like the
    // relocate it generalizes. Undone by moveSegment(to, insertAt, len, from,
    // start).
    void moveSegment(ScoreDirector<Solution>& director, int from, int start, int len, int to,
                     int at) {
        scratch1_.resize(static_cast<std::size_t>(len));
        for (int k = 0; k < len; ++k) {
            scratch1_[static_cast<std::size_t>(k)] = removeAt(director, from, start);
        }
        for (int k = 0; k < len; ++k) {
            insertAt(director, to, at + k, scratch1_[static_cast<std::size_t>(k)]);
        }
    }

    void swapElements(ScoreDirector<Solution>& director, int list1, int pos1, int list2, int pos2,
                      int element1, int element2) {
        setElement(director, list1, pos1, element2);
        setElement(director, list2, pos2, element1);
        std::swap(list_[element1], list_[element2]);
        std::swap(position_[element1], position_[element2]);
    }

    // Exchange the tails of two distinct lists from the given cuts. Self-
    // inverse with the same cuts; one hook pair per list.
    void swapTails(ScoreDirector<Solution>& director, int l1, int c1, int l2, int c2) {
        Solution& solution = director.workingSolution();
        scratch1_.clear();
        scratch2_.clear();
        for (int p = c1; p < Descriptor::listSize(solution, l1); ++p) {
            scratch1_.push_back(Descriptor::getElement(solution, l1, p));
        }
        for (int p = c2; p < Descriptor::listSize(solution, l2); ++p) {
            scratch2_.push_back(Descriptor::getElement(solution, l2, p));
        }
        replaceTail(director, l1, c1, scratch2_);
        replaceTail(director, l2, c2, scratch1_);
        reindex(solution, l1);
        reindex(solution, l2);
    }

    // Swap the run of `len1` elements at (l1, i1) with the run of `len2` at
    // (l2, i2), across two distinct lists. Undone by the same call with the
    // lengths swapped: afterwards l1 holds len2 elements at i1 and l2 holds
    // len1 at i2.
    void exchangeSegments(ScoreDirector<Solution>& director, int l1, int i1, int len1, int l2,
                          int i2, int len2) {
        Solution& solution = director.workingSolution();
        scratch1_.clear();
        scratch2_.clear();
        for (int k = 0; k < len1; ++k) {
            scratch1_.push_back(Descriptor::getElement(solution, l1, i1 + k));
        }
        for (int k = 0; k < len2; ++k) {
            scratch2_.push_back(Descriptor::getElement(solution, l2, i2 + k));
        }
        director.beginListChange(l1);
        for (int k = 0; k < len1; ++k) Descriptor::removeElement(solution, l1, i1);
        for (int k = 0; k < len2; ++k) {
            Descriptor::insertElement(solution, l1, i1 + k, scratch2_[static_cast<std::size_t>(k)]);
        }
        director.endListChange(l1);
        director.beginListChange(l2);
        for (int k = 0; k < len2; ++k) Descriptor::removeElement(solution, l2, i2);
        for (int k = 0; k < len1; ++k) {
            Descriptor::insertElement(solution, l2, i2 + k, scratch1_[static_cast<std::size_t>(k)]);
        }
        director.endListChange(l2);
        reindex(solution, l1);
        reindex(solution, l2);
    }

    // Replace the element at a position (one coherent list change).
    static void setElement(ScoreDirector<Solution>& director, int list, int position,
                           int element) {
        director.beginListChange(list);
        Descriptor::removeElement(director.workingSolution(), list, position);
        Descriptor::insertElement(director.workingSolution(), list, position, element);
        director.endListChange(list);
    }

private:
    static void replaceTail(ScoreDirector<Solution>& director, int list, int cut,
                            const std::vector<int>& tail) {
        Solution& solution = director.workingSolution();
        director.beginListChange(list);
        while (Descriptor::listSize(solution, list) > cut) {
            Descriptor::removeElement(solution, list, Descriptor::listSize(solution, list) - 1);
        }
        for (int e : tail) {
            Descriptor::insertElement(solution, list, Descriptor::listSize(solution, list), e);
        }
        director.endListChange(list);
    }

    std::vector<int> list_;
    std::vector<int> position_;
    std::vector<int> scratch1_, scratch2_;  // swapTails / exchangeSegments buffers
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_ELEMENT_INDEX_HPP

// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_ELEMENT_INDEX_HPP
#define OPTIMARU_SEARCH_ELEMENT_INDEX_HPP

#include <utility>
#include <vector>

#include "optimaru/solution_traits.hpp"
#include "optimaru/editor.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// ElementIndex (sequence shape)
// ---------------------------------------------------------------------------
// element id -> (list, position), so "where is element e right now?" is O(1)
// instead of a scan over every list. A list of -1 means the element is
// currently unassigned, which happens only while a ruin is in flight.
//
// The engine mutates lists through this class rather than through the
// Editor directly: the editor keeps the SCORE correct, this keeps the
// INDEX correct, and the two must move together. (A custom MoveOperator that
// writes straight through the editor is a legitimate exception — local search
// resynchronizes afterwards from Editor::touched_lists(). The built-in
// strategies do not take that route, because they need the index to be live
// mid-move: their own repair reads it to find nearby anchors.)
template <typename Solution>
class ElementIndex {
    using Traits = SolutionTraits<Solution>;

public:
    void build(const Solution& solution) {
        list_.assign(Traits::element_count(solution), -1);
        position_.assign(Traits::element_count(solution), -1);
        for (int l = 0; l < Traits::list_count(solution); ++l) reindex(solution, l);
    }

    void reindex(const Solution& solution, int list) {
        const int size = Traits::list_size(solution, list);
        for (int p = 0; p < size; ++p) {
            const int e = Traits::element_at(solution, list, p);
            list_[e] = list;
            position_[e] = p;
        }
    }

    int list_of(int element) const { return list_[element]; }
    int position_of(int element) const { return position_[element]; }

    // Exposed as vectors because OperatorContext hands them to custom
    // operators as read-only spans.
    const std::vector<int>& lists() const { return list_; }
    const std::vector<int>& positions() const { return position_; }

    int remove_at(Editor<Solution>& editor, int list, int position) {
        const int e = editor.remove(list, position);
        list_[e] = -1;
        position_[e] = -1;
        reindex(editor.solution(), list);
        return e;
    }

    void insert_at(Editor<Solution>& editor, int list, int position, int element) {
        editor.insert(list, position, element);
        reindex(editor.solution(), list);
    }

    void reverse(Editor<Solution>& editor, int list, int first, int last) {
        editor.reverse(list, first, last);
        reindex(editor.solution(), list);
    }

    // Move the `len` elements at [start, start+len) of `from` to `insert_at`
    // of `to`, in order; `insert_at` indexes `to` after the segment is out
    // (the lists may be the same). One hook pair per element, like the
    // relocate it generalizes. Undone by move_segment(to, insert_at, len, from,
    // start).
    void move_segment(Editor<Solution>& editor, int from, int start, int len, int to,
                     int at) {
        scratch1_.resize(static_cast<std::size_t>(len));
        for (int k = 0; k < len; ++k) {
            scratch1_[static_cast<std::size_t>(k)] = remove_at(editor, from, start);
        }
        for (int k = 0; k < len; ++k) {
            insert_at(editor, to, at + k, scratch1_[static_cast<std::size_t>(k)]);
        }
    }

    void swap_elements(Editor<Solution>& editor, int list1, int pos1, int list2, int pos2,
                      int element1, int element2) {
        set_element(editor, list1, pos1, element2);
        set_element(editor, list2, pos2, element1);
        std::swap(list_[element1], list_[element2]);
        std::swap(position_[element1], position_[element2]);
    }

    // Exchange the tails of two distinct lists from the given cuts. Self-
    // inverse with the same cuts; one hook pair per list.
    void swap_tails(Editor<Solution>& editor, int l1, int c1, int l2, int c2) {
        Solution& solution = editor.solution();
        scratch1_.clear();
        scratch2_.clear();
        for (int p = c1; p < Traits::list_size(solution, l1); ++p) {
            scratch1_.push_back(Traits::element_at(solution, l1, p));
        }
        for (int p = c2; p < Traits::list_size(solution, l2); ++p) {
            scratch2_.push_back(Traits::element_at(solution, l2, p));
        }
        replace_tail(editor, l1, c1, scratch2_);
        replace_tail(editor, l2, c2, scratch1_);
        reindex(solution, l1);
        reindex(solution, l2);
    }

    // Swap the run of `len1` elements at (l1, i1) with the run of `len2` at
    // (l2, i2), across two distinct lists. Undone by the same call with the
    // lengths swapped: afterwards l1 holds len2 elements at i1 and l2 holds
    // len1 at i2.
    void exchange_segments(Editor<Solution>& editor, int l1, int i1, int len1, int l2,
                          int i2, int len2) {
        Solution& solution = editor.solution();
        scratch1_.clear();
        scratch2_.clear();
        for (int k = 0; k < len1; ++k) {
            scratch1_.push_back(Traits::element_at(solution, l1, i1 + k));
        }
        for (int k = 0; k < len2; ++k) {
            scratch2_.push_back(Traits::element_at(solution, l2, i2 + k));
        }
        editor.begin_list_edit(l1);
        for (int k = 0; k < len1; ++k) Traits::remove_element(solution, l1, i1);
        for (int k = 0; k < len2; ++k) {
            Traits::insert_element(solution, l1, i1 + k, scratch2_[static_cast<std::size_t>(k)]);
        }
        editor.end_list_edit(l1);
        editor.begin_list_edit(l2);
        for (int k = 0; k < len2; ++k) Traits::remove_element(solution, l2, i2);
        for (int k = 0; k < len1; ++k) {
            Traits::insert_element(solution, l2, i2 + k, scratch1_[static_cast<std::size_t>(k)]);
        }
        editor.end_list_edit(l2);
        reindex(solution, l1);
        reindex(solution, l2);
    }

    // Replace the element at a position (one coherent list change).
    static void set_element(Editor<Solution>& editor, int list, int position,
                           int element) {
        editor.begin_list_edit(list);
        Traits::remove_element(editor.solution(), list, position);
        Traits::insert_element(editor.solution(), list, position, element);
        editor.end_list_edit(list);
    }

private:
    static void replace_tail(Editor<Solution>& editor, int list, int cut,
                            const std::vector<int>& tail) {
        Solution& solution = editor.solution();
        editor.begin_list_edit(list);
        while (Traits::list_size(solution, list) > cut) {
            Traits::remove_element(solution, list, Traits::list_size(solution, list) - 1);
        }
        for (int e : tail) {
            Traits::insert_element(solution, list, Traits::list_size(solution, list), e);
        }
        editor.end_list_edit(list);
    }

    std::vector<int> list_;
    std::vector<int> position_;
    std::vector<int> scratch1_, scratch2_;  // swap_tails / exchange_segments buffers
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_ELEMENT_INDEX_HPP

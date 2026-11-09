// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_TESTING_PREVIEW_PARITY_HPP
#define OPTIMARU_TESTING_PREVIEW_PARITY_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "optimaru/solution_traits.hpp"
#include "optimaru/objective.hpp"
#include "optimaru/editor.hpp"

namespace optimaru::testing {

// ---------------------------------------------------------------------------
// Preview parity harness (sequence shape)
// ---------------------------------------------------------------------------
// Draws random moves of every previewable kind on the editor's working
// solution, asks the objective for the preview, then performs the move for
// real through the editor, compares score() with the preview, and
// undoes the move. For a use case this is the one test every objective with
// previews needs; the engine's own ScoreChecks::Full runs the same comparison on
// every preview during a solve.
//
// `trials` draws are attempted; a draw that the current list sizes cannot
// support, or that the objective declines, is not counted as checked.
struct PreviewParity {
    int checked = 0;
    int previewed = 0;   // previews served (== checked)
    int declined = 0;    // previews the objective refused
    int mismatches = 0;  // previews that differ from the applied score
    bool all_equal() const { return mismatches == 0; }
};

template <typename Solution>
PreviewParity run_preview_parity(Editor<Solution>& editor,
                               Objective<Solution>& objective,
                               std::uint64_t seed, int trials) {
    using Traits = SolutionTraits<Solution>;
    static_assert(Traits::shape == Shape::Sequence, "previews are a sequence-shape feature");
    Solution& solution = editor.solution();
    std::mt19937_64 rng(seed);
    const int list_count = Traits::list_count(solution);
    auto size = [&](int l) { return Traits::list_size(solution, l); };
    auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<std::uint64_t>(n)); };

    PreviewParity result;
    auto compare = [&](const Score& preview, const Score& actual,
                       const char* kind) {
        ++result.checked;
        ++result.previewed;
        if (!scores_agree(preview, actual)) {
            if (++result.mismatches <= 5) {
                std::printf("    %s: preview %s != actual %s\n", kind,
                            preview.str().c_str(), actual.str().c_str());
            }
        }
    };
    // Replace the element at (l, p) through the editor's hook protocol.
    auto set = [&](int l, int p, int e) {
        editor.begin_list_edit(l);
        Traits::remove_element(solution, l, p);
        Traits::insert_element(solution, l, p, e);
        editor.end_list_edit(l);
    };
    auto set_tail = [&](int l, int cut, const std::vector<int>& tail) {
        editor.begin_list_edit(l);
        while (Traits::list_size(solution, l) > cut)
            Traits::remove_element(solution, l, Traits::list_size(solution, l) - 1);
        for (int e : tail) Traits::insert_element(solution, l, Traits::list_size(solution, l), e);
        editor.end_list_edit(l);
    };

    Score preview;
    for (int trial = 0; trial < trials; ++trial) {
        const int kind = pick(6);
        if (kind == 5) {  // insert of a detached element (the repair heuristics' preview)
            const int from = pick(list_count);
            if (size(from) == 0) continue;
            const int p = pick(size(from));
            const int e = editor.remove(from, p);
            const int to = pick(list_count);
            const int at = pick(size(to) + 1);
            if (objective.preview_insert(to, at, e, preview)) {
                editor.insert(to, at, e);
                compare(preview, editor.score(), "insert");
                editor.remove(to, at);
            } else {
                ++result.declined;
            }
            editor.insert(from, p, e);
        } else if (kind == 0) {  // segment move (relocate / Or-opt), possibly within one list
            const int from = pick(list_count);
            if (size(from) == 0) continue;
            const int len = 1 + pick(std::min(3, size(from)));
            const int start = pick(size(from) - len + 1);
            const int to = pick(list_count);
            const int reduced = size(to) - (to == from ? len : 0);
            const int insert_at = pick(reduced + 1);
            if (!objective.preview_segment_move(from, start, len, to, insert_at, preview)) {
                ++result.declined;
                continue;
            }
            std::vector<int> seg;
            for (int k = 0; k < len; ++k) seg.push_back(editor.remove(from, start));
            for (int k = 0; k < len; ++k) editor.insert(to, insert_at + k, seg[k]);
            compare(preview, editor.score(), "segment move");
            for (int k = 0; k < len; ++k) editor.remove(to, insert_at);
            for (int k = 0; k < len; ++k) editor.insert(from, start + k, seg[k]);
        } else if (kind == 1) {  // swap, same or different lists
            const int l1 = pick(list_count), l2 = pick(list_count);
            if (size(l1) == 0 || size(l2) == 0) continue;
            const int p1 = pick(size(l1)), p2 = pick(size(l2));
            if (l1 == l2 && p1 == p2) continue;
            if (!objective.preview_swap(l1, p1, l2, p2, preview)) {
                ++result.declined;
                continue;
            }
            const int e1 = Traits::element_at(solution, l1, p1);
            const int e2 = Traits::element_at(solution, l2, p2);
            set(l1, p1, e2);
            set(l2, p2, e1);
            compare(preview, editor.score(), "swap");
            set(l1, p1, e1);
            set(l2, p2, e2);
        } else if (kind == 2) {  // reverse
            const int l = pick(list_count);
            if (size(l) < 2) continue;
            const int a = pick(size(l)), b = pick(size(l));
            if (a == b) continue;
            const int first = std::min(a, b), last = std::max(a, b);
            if (!objective.preview_reverse(l, first, last, preview)) {
                ++result.declined;
                continue;
            }
            editor.reverse(l, first, last);
            compare(preview, editor.score(), "reverse");
            editor.reverse(l, first, last);
        } else if (kind == 3) {  // tail swap
            if (list_count < 2) continue;
            const int l1 = pick(list_count);
            int l2 = pick(list_count - 1);
            if (l2 >= l1) ++l2;
            const int c1 = pick(size(l1) + 1), c2 = pick(size(l2) + 1);
            if (!objective.preview_tail_swap(l1, c1, l2, c2, preview)) {
                ++result.declined;
                continue;
            }
            std::vector<int> tail1, tail2;
            for (int p = c1; p < size(l1); ++p) tail1.push_back(Traits::element_at(solution, l1, p));
            for (int p = c2; p < size(l2); ++p) tail2.push_back(Traits::element_at(solution, l2, p));
            set_tail(l1, c1, tail2);
            set_tail(l2, c2, tail1);
            compare(preview, editor.score(), "tail swap");
            set_tail(l1, c1, tail1);
            set_tail(l2, c2, tail2);
        } else {  // CROSS exchange with lengths 0..3
            if (list_count < 2) continue;
            const int l1 = pick(list_count);
            int l2 = pick(list_count - 1);
            if (l2 >= l1) ++l2;
            const int p1 = pick(size(l1) + 1), p2 = pick(size(l2) + 1);
            const int len1 = pick(std::min(3, size(l1) - p1) + 1);
            const int len2 = pick(std::min(3, size(l2) - p2) + 1);
            if (len1 == 0 && len2 == 0) continue;
            if (!objective.preview_exchange(l1, p1, len1, l2, p2, len2, preview)) {
                ++result.declined;
                continue;
            }
            std::vector<int> s1, s2;
            for (int k = 0; k < len1; ++k) s1.push_back(Traits::element_at(solution, l1, p1 + k));
            for (int k = 0; k < len2; ++k) s2.push_back(Traits::element_at(solution, l2, p2 + k));
            auto exchange = [&](const std::vector<int>& a, const std::vector<int>& b) {
                editor.begin_list_edit(l1);
                for (std::size_t k = 0; k < a.size(); ++k) Traits::remove_element(solution, l1, p1);
                for (std::size_t k = 0; k < b.size(); ++k)
                    Traits::insert_element(solution, l1, p1 + static_cast<int>(k), b[k]);
                editor.end_list_edit(l1);
                editor.begin_list_edit(l2);
                for (std::size_t k = 0; k < b.size(); ++k) Traits::remove_element(solution, l2, p2);
                for (std::size_t k = 0; k < a.size(); ++k)
                    Traits::insert_element(solution, l2, p2 + static_cast<int>(k), a[k]);
                editor.end_list_edit(l2);
            };
            exchange(s1, s2);
            compare(preview, editor.score(), "exchange");
            exchange(s2, s1);
        }
    }
    return result;
}

}  // namespace optimaru::testing

#endif  // OPTIMARU_TESTING_PREVIEW_PARITY_HPP

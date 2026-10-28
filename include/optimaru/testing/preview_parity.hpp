#ifndef OPTIMARU_TESTING_PREVIEW_PARITY_HPP
#define OPTIMARU_TESTING_PREVIEW_PARITY_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"

namespace optimaru::testing {

// ---------------------------------------------------------------------------
// Preview parity harness (LIST kind)
// ---------------------------------------------------------------------------
// Draws random moves of every previewable kind on the director's working
// solution, asks the calculator for the preview, then performs the move for
// real through the director, compares calculateScore() with the preview, and
// undoes the move. For a use case this is the one test every calculator with
// previews needs; the engine's own FULL_ASSERT runs the same comparison on
// every preview during a solve.
//
// `trials` draws are attempted; a draw that the current list sizes cannot
// support, or that the calculator declines, is not counted as checked.
struct PreviewParity {
    int checked = 0;
    int previewed = 0;   // previews served (== checked)
    int declined = 0;    // previews the calculator refused
    int mismatches = 0;  // previews that differ from the applied score
    bool allEqual() const { return mismatches == 0; }
};

template <typename Solution>
PreviewParity runPreviewParity(ScoreDirector<Solution>& director,
                               IncrementalScoreCalculator<Solution>& calculator,
                               std::uint64_t seed, int trials) {
    using Descriptor = SolutionDescriptor<Solution>;
    static_assert(Descriptor::kind == VariableKind::LIST, "previews are a LIST-kind feature");
    Solution& solution = director.workingSolution();
    std::mt19937_64 rng(seed);
    const int listCount = Descriptor::listCount(solution);
    auto size = [&](int l) { return Descriptor::listSize(solution, l); };
    auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<std::uint64_t>(n)); };

    PreviewParity result;
    auto compare = [&](const HardSoftScore& preview, const HardSoftScore& actual,
                       const char* kind) {
        ++result.checked;
        ++result.previewed;
        if (!scoresAgree(preview, actual)) {
            if (++result.mismatches <= 5) {
                std::printf("    %s: preview %s != actual %s\n", kind,
                            preview.toString().c_str(), actual.toString().c_str());
            }
        }
    };
    // Replace the element at (l, p) through the director's hook protocol.
    auto set = [&](int l, int p, int e) {
        director.beginListChange(l);
        Descriptor::removeElement(solution, l, p);
        Descriptor::insertElement(solution, l, p, e);
        director.endListChange(l);
    };
    auto setTail = [&](int l, int cut, const std::vector<int>& tail) {
        director.beginListChange(l);
        while (Descriptor::listSize(solution, l) > cut)
            Descriptor::removeElement(solution, l, Descriptor::listSize(solution, l) - 1);
        for (int e : tail) Descriptor::insertElement(solution, l, Descriptor::listSize(solution, l), e);
        director.endListChange(l);
    };

    HardSoftScore preview;
    for (int trial = 0; trial < trials; ++trial) {
        const int kind = pick(6);
        if (kind == 5) {  // insert of a detached element (the repair heuristics' preview)
            const int from = pick(listCount);
            if (size(from) == 0) continue;
            const int p = pick(size(from));
            const int e = director.removeListElement(from, p);
            const int to = pick(listCount);
            const int at = pick(size(to) + 1);
            if (calculator.previewInsert(to, at, e, preview)) {
                director.insertListElement(to, at, e);
                compare(preview, director.calculateScore(), "insert");
                director.removeListElement(to, at);
            } else {
                ++result.declined;
            }
            director.insertListElement(from, p, e);
        } else if (kind == 0) {  // segment move (relocate / Or-opt), possibly within one list
            const int from = pick(listCount);
            if (size(from) == 0) continue;
            const int len = 1 + pick(std::min(3, size(from)));
            const int start = pick(size(from) - len + 1);
            const int to = pick(listCount);
            const int reduced = size(to) - (to == from ? len : 0);
            const int insertAt = pick(reduced + 1);
            if (!calculator.previewSegmentMove(from, start, len, to, insertAt, preview)) {
                ++result.declined;
                continue;
            }
            std::vector<int> seg;
            for (int k = 0; k < len; ++k) seg.push_back(director.removeListElement(from, start));
            for (int k = 0; k < len; ++k) director.insertListElement(to, insertAt + k, seg[k]);
            compare(preview, director.calculateScore(), "segment move");
            for (int k = 0; k < len; ++k) director.removeListElement(to, insertAt);
            for (int k = 0; k < len; ++k) director.insertListElement(from, start + k, seg[k]);
        } else if (kind == 1) {  // swap, same or different lists
            const int l1 = pick(listCount), l2 = pick(listCount);
            if (size(l1) == 0 || size(l2) == 0) continue;
            const int p1 = pick(size(l1)), p2 = pick(size(l2));
            if (l1 == l2 && p1 == p2) continue;
            if (!calculator.previewSwap(l1, p1, l2, p2, preview)) {
                ++result.declined;
                continue;
            }
            const int e1 = Descriptor::getElement(solution, l1, p1);
            const int e2 = Descriptor::getElement(solution, l2, p2);
            set(l1, p1, e2);
            set(l2, p2, e1);
            compare(preview, director.calculateScore(), "swap");
            set(l1, p1, e1);
            set(l2, p2, e2);
        } else if (kind == 2) {  // reverse
            const int l = pick(listCount);
            if (size(l) < 2) continue;
            const int a = pick(size(l)), b = pick(size(l));
            if (a == b) continue;
            const int first = std::min(a, b), last = std::max(a, b);
            if (!calculator.previewReverse(l, first, last, preview)) {
                ++result.declined;
                continue;
            }
            director.reverseListSegment(l, first, last);
            compare(preview, director.calculateScore(), "reverse");
            director.reverseListSegment(l, first, last);
        } else if (kind == 3) {  // tail swap
            if (listCount < 2) continue;
            const int l1 = pick(listCount);
            int l2 = pick(listCount - 1);
            if (l2 >= l1) ++l2;
            const int c1 = pick(size(l1) + 1), c2 = pick(size(l2) + 1);
            if (!calculator.previewTailSwap(l1, c1, l2, c2, preview)) {
                ++result.declined;
                continue;
            }
            std::vector<int> tail1, tail2;
            for (int p = c1; p < size(l1); ++p) tail1.push_back(Descriptor::getElement(solution, l1, p));
            for (int p = c2; p < size(l2); ++p) tail2.push_back(Descriptor::getElement(solution, l2, p));
            setTail(l1, c1, tail2);
            setTail(l2, c2, tail1);
            compare(preview, director.calculateScore(), "tail swap");
            setTail(l1, c1, tail1);
            setTail(l2, c2, tail2);
        } else {  // CROSS exchange with lengths 0..3
            if (listCount < 2) continue;
            const int l1 = pick(listCount);
            int l2 = pick(listCount - 1);
            if (l2 >= l1) ++l2;
            const int p1 = pick(size(l1) + 1), p2 = pick(size(l2) + 1);
            const int len1 = pick(std::min(3, size(l1) - p1) + 1);
            const int len2 = pick(std::min(3, size(l2) - p2) + 1);
            if (len1 == 0 && len2 == 0) continue;
            if (!calculator.previewExchange(l1, p1, len1, l2, p2, len2, preview)) {
                ++result.declined;
                continue;
            }
            std::vector<int> s1, s2;
            for (int k = 0; k < len1; ++k) s1.push_back(Descriptor::getElement(solution, l1, p1 + k));
            for (int k = 0; k < len2; ++k) s2.push_back(Descriptor::getElement(solution, l2, p2 + k));
            auto exchange = [&](const std::vector<int>& a, const std::vector<int>& b) {
                director.beginListChange(l1);
                for (std::size_t k = 0; k < a.size(); ++k) Descriptor::removeElement(solution, l1, p1);
                for (std::size_t k = 0; k < b.size(); ++k)
                    Descriptor::insertElement(solution, l1, p1 + static_cast<int>(k), b[k]);
                director.endListChange(l1);
                director.beginListChange(l2);
                for (std::size_t k = 0; k < b.size(); ++k) Descriptor::removeElement(solution, l2, p2);
                for (std::size_t k = 0; k < a.size(); ++k)
                    Descriptor::insertElement(solution, l2, p2 + static_cast<int>(k), a[k]);
                director.endListChange(l2);
            };
            exchange(s1, s2);
            compare(preview, director.calculateScore(), "exchange");
            exchange(s2, s1);
        }
    }
    return result;
}

}  // namespace optimaru::testing

#endif  // OPTIMARU_TESTING_PREVIEW_PARITY_HPP

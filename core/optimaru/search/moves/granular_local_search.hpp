// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_MOVES_GRANULAR_LOCAL_SEARCH_HPP
#define OPTIMARU_SEARCH_MOVES_GRANULAR_LOCAL_SEARCH_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include "optimaru/model/editor.hpp"
#include "optimaru/model/element_index.hpp"
#include "optimaru/model/neighborhoods.hpp"
#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/search/moves/list_move.hpp"
#include "optimaru/stop/stop_throttle.hpp"
#include "optimaru/utils/random.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// GranularLocalSearch — first-improvement descent to a local optimum (sequence)
// ---------------------------------------------------------------------------
// The neighbourhood of the HGS / SISR / AILS family of VRP solvers: for every
// element u and every nearby v, the handful of moves that reconnect u next to
// v — relocate (before/after), swap, 2-opt within a list, 2-opt* across lists,
// Or-opt segments and small CROSS exchanges — plus the relocation of u into an
// empty list. The first improving move is applied and the search goes on
// until no element has one left ("don't look bits": an element is only
// re-examined after one of the lists it touches has changed).
//
// Every candidate is scored through the objective's previews, so one
// evaluation is O(1) on an objective that implements them, and the whole
// descent is O(elements x nearby) per pass. Without previews it still works
// (apply / score / undo), just slowly.
//
// The excluded list (an unassigned pool) is neither a source nor a target:
// moving elements in or out of it is the repair's and the ejection chain's
// job, where hard feasibility is judged as a whole.
template <typename Solution>
class GranularLocalSearch {
    using Traits = SolutionTraits<Solution>;

public:
    GranularLocalSearch(ElementIndex<Solution>& index, const Neighborhoods& nearby, int max_segment)
        : index_(index),
          nearby_(nearby),
          max_segment_(std::clamp(max_segment, 1, ListMove::kMaxSegment)) {}

    // Every element is re-examined on the next run().
    void mark_all(const Solution& solution) {
        dirty_.assign(static_cast<std::size_t>(Traits::element_count(solution)), 1);
    }

    // The elements of `list` are re-examined on the next run().
    void mark_list(const Solution& solution, int list) {
        if (list < 0) return;
        dirty_.resize(static_cast<std::size_t>(Traits::element_count(solution)), 1);
        const int size = Traits::list_size(solution, list);
        for (int p = 0; p < size; ++p) {
            dirty_[static_cast<std::size_t>(Traits::element_at(solution, list, p))] = 1;
        }
    }

    // Descend until no marked element has an improving move, or the
    // evaluation budget is spent. Returns the number of moves applied.
    // `stop_requested` (may be null) is the solver's stop condition, consulted
    // through a StopThrottle: a preview on a plan with waiting or on a
    // decomposed objective costs O(route), so the evaluation budget alone
    // would let one descent run seconds past the time limit.
    int run(Editor<Solution>& editor, std::mt19937_64& rng, int exclude_list,
            std::int64_t max_evaluations, const std::function<bool()>* stop_requested = nullptr) {
        if constexpr (Traits::shape != Shape::Sequence) {
            (void)editor;
            (void)rng;
            (void)exclude_list;
            (void)max_evaluations;
            (void)stop_requested;
            return 0;
        } else {
            if (!nearby_.enabled()) return 0;
            const Solution& solution = editor.solution();
            const int element_count = Traits::element_count(solution);
            dirty_.resize(static_cast<std::size_t>(element_count), 1);
            order_.resize(static_cast<std::size_t>(element_count));
            for (int e = 0; e < element_count; ++e) order_[static_cast<std::size_t>(e)] = e;
            evaluations_ = 0;
            declined_ = 0;
            exclude_list_ = exclude_list;
            int applied = 0;
            bool any = true;
            // Declined previews are scored by applying the move, O(list) each;
            // a descent that keeps hitting them stops early so one iteration
            // cannot run far past the time limit, which is only read between
            // steps. The stop condition itself is asked through the throttle
            // for the same reason.
            throttle_.reset(stop_requested);
            auto exhausted = [&] {
                return throttle_.reached(evaluations_) || evaluations_ >= max_evaluations ||
                       declined_ >= kMaxDeclined;
            };
            while (any && !exhausted()) {
                any = false;
                random_shuffle(order_.begin(), order_.end(), rng);
                for (int u : order_) {
                    if (exhausted()) break;
                    if (!dirty_[static_cast<std::size_t>(u)]) continue;
                    dirty_[static_cast<std::size_t>(u)] = 0;
                    const int lu = index_.list_of(u);
                    if (lu < 0 || lu == exclude_list) continue;
                    if (improve_around(editor, u)) {
                        ++applied;
                        any = true;
                    }
                }
            }
            return applied;
        }
    }

    std::int64_t declined() const { return declined_; }

    // Slow-path (declined-preview) evaluations one run() tolerates: at an
    // O(list) apply/score/undo each this is on the order of a tenth of a
    // second on 25-stop routes.
    static constexpr std::int64_t kMaxDeclined = 20000;

private:
    using Moves = ListMoves<Solution>;

    // Score of the solution after `m` (see ListMoves::evaluate); a declined
    // preview is counted against kMaxDeclined.
    Score evaluate(Editor<Solution>& editor, const ListMove& m) {
        ++evaluations_;
        bool served = false;
        const Score score = Moves::evaluate(editor, index_, m, &served);
        if (!served) ++declined_;
        return score;
    }

    // The first improving move around u, applied. Both lists it touched are
    // marked so their elements get another look.
    bool improve_around(Editor<Solution>& editor, int u) {
        const Solution& solution = editor.solution();
        const Score current = editor.score();
        const int lu = index_.list_of(u);
        const int pu = index_.position_of(u);
        const int su = Traits::list_size(solution, lu);

        auto try_move = [&](const ListMove& m) {
            if (evaluate(editor, m) > current) {
                Moves::apply(editor, index_, m);
                mark_list(editor.solution(), m.l1);
                mark_list(editor.solution(), m.l2);
                return true;
            }
            return false;
        };

        // Into an empty list: the one move no neighbour of u suggests. Every
        // empty list is tried, since lists need not be interchangeable (a
        // heterogeneous fleet).
        const int list_count = Traits::list_count(solution);
        for (int l = 0; l < list_count; ++l) {
            if (l == lu || l == exclude_list_ || Traits::list_size(solution, l) != 0) continue;
            if (try_move(ListMove::segment(lu, pu, 1, l, 0))) return true;
        }

        for (int v : nearby_.of(u)) {
            const int lv = index_.list_of(v);
            if (lv < 0 || lv == exclude_list_) continue;
            const int pv = index_.position_of(v);
            const int sv = Traits::list_size(solution, lv);
            const bool same = lu == lv;

            // Relocate u after v, and before v.
            {
                const int after = same ? (pv < pu ? pv + 1 : pv) : pv + 1;
                if (!(same && after == pu)) {
                    if (try_move(ListMove::segment(lu, pu, 1, lv, after))) return true;
                }
                const int before = same ? (pv < pu ? pv : pv - 1) : pv;
                if (!(same && before == pu)) {
                    if (try_move(ListMove::segment(lu, pu, 1, lv, before))) return true;
                }
            }
            // Swap u and v.
            if (try_move(ListMove::swap(lu, pu, lv, pv))) return true;

            if (same) {
                // 2-opt: reverse the stretch between u and v, either way round.
                const int a = std::min(pu, pv), b = std::max(pu, pv);
                if (b - a >= 2) {
                    if (try_move(ListMove::reverse(lu, a + 1, b))) return true;
                    if (try_move(ListMove::reverse(lu, a, b - 1))) return true;
                }
            } else {
                // 2-opt*: exchange the tails after u and v, or before them.
                if (pu + 1 < su || pv + 1 < sv) {
                    if (try_move(ListMove::tail_swap(lu, pu + 1, lv, pv + 1))) return true;
                }
                if (try_move(ListMove::tail_swap(lu, pu, lv, pv))) return true;
                // CROSS: the pair starting at u against v, or v's pair.
                if (pu + 1 < su) {
                    if (try_move(ListMove::exchange(lu, pu, 2, lv, pv, 1))) return true;
                    if (pv + 1 < sv) {
                        if (try_move(ListMove::exchange(lu, pu, 2, lv, pv, 2))) return true;
                    }
                }
                if (pv + 1 < sv) {
                    if (try_move(ListMove::exchange(lu, pu, 1, lv, pv, 2))) return true;
                }
            }
            // Or-opt: the segment starting at u, relocated after v.
            for (int len = 2; len <= max_segment_ && pu + len <= su; ++len) {
                if (same && pv >= pu && pv < pu + len) continue;  // v inside the segment
                const int at = same ? (pv < pu ? pv + 1 : pv + 1 - len) : pv + 1;
                if (same && at == pu) continue;
                if (try_move(ListMove::segment(lu, pu, len, lv, at))) return true;
            }
        }
        return false;
    }

    ElementIndex<Solution>& index_;
    const Neighborhoods& nearby_;
    int max_segment_;
    int exclude_list_ = -1;
    std::int64_t evaluations_ = 0;
    StopThrottle throttle_;
    std::int64_t declined_ = 0;
    std::vector<char> dirty_;
    std::vector<int> order_;
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_MOVES_GRANULAR_LOCAL_SEARCH_HPP

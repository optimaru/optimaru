// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_MOVES_ITERATED_LOCAL_SEARCH_HPP
#define OPTIMARU_MOVES_ITERATED_LOCAL_SEARCH_HPP

#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/model/neighborhoods.hpp"
#include "optimaru/select/scheme.hpp"
#include "optimaru/model/element_index.hpp"
#include "optimaru/ruin_recreate/greedy_repair.hpp"
#include "optimaru/ruin_recreate/removal_gain.hpp"
#include "optimaru/moves/list_move.hpp"
#include "optimaru/stop/stop_throttle.hpp"
#include "optimaru/model/snapshot.hpp"
#include "optimaru/search/operators.hpp"
#include "optimaru/portable_random.hpp"

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
    GranularLocalSearch(ElementIndex<Solution>& index, const Neighborhoods& nearby,
                        int max_segment)
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
                std::shuffle(order_.begin(), order_.end(), rng);
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

// ---------------------------------------------------------------------------
// IteratedLocalSearchMove — ruin, recreate, descend to a local optimum
// ---------------------------------------------------------------------------
// One iteration of an iterated local search, entered as an ordinary candidate
// move: a small ruin (drawn from the adaptive ruin pool), the greedy repair,
// then GranularLocalSearch over the lists the repair touched until they are
// locally optimal again. The acceptor judges the local optimum, and a
// rejected iteration is undone from a snapshot.
//
// The first iteration skips the ruin and descends from the current solution
// as it is, so the construction heuristic's output is a local optimum before
// anything is perturbed.
//
// The ruin selector is the operator's own: an outcome here is judged after
// the descent, so it must not retrain the weights the raw destroy-repair move
// and the stagnation kick are judged on.
//
// The operator declines on an objective whose has_previews() is false
// (the default implementation): the same descent through apply/score/undo
// would take seconds per iteration for nothing the ordinary moves cannot do.
// One that previews in general but declines most moves for the plan at hand
// is backed off from for a while after two descents in a row were cut by
// GranularLocalSearch::kMaxDeclined.
template <typename Solution>
class IteratedLocalSearchMove final : public MoveOperator<Solution> {
    using Traits = SolutionTraits<Solution>;
    using Snapshot = SolutionSnapshot<Solution>;

public:
    IteratedLocalSearchMove(ElementIndex<Solution>& index, GreedyRepair<Solution>& repair,
                            GranularLocalSearch<Solution>& local_search,
                            const std::vector<RuinOperator<Solution>*>& ruins,
                            OperatorSelectionScheme& ruin_selector, int min_ruin, int max_ruin,
                            int exclude_list, std::int64_t max_evaluations)
        : index_(index),
          repair_(repair),
          local_search_(local_search),
          ruins_(ruins),
          ruin_selector_(ruin_selector),
          min_ruin_(std::max(1, min_ruin)),
          max_ruin_(std::max(min_ruin_, max_ruin)),
          exclude_list_(exclude_list),
          max_evaluations_(max_evaluations) {}

    const char* name() const override { return "iteratedLocalSearch"; }

    bool apply(OperatorContext<Solution>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        if constexpr (Traits::shape != Shape::Sequence) {
            (void)ctx;
            (void)rng;
            (void)entities;
            return false;
        } else {
            Editor<Solution>& editor = ctx.editor;
            Solution& solution = editor.solution();
            const int n = ruin_candidate_count(ctx);
            if (n < 2 || ruins_.empty() || !ctx.nearby.enabled()) return false;
            if (!editor.objective().has_previews()) return false;
            // An objective that says it previews but declines most moves for
            // this plan sends the descent down the apply/score/undo path until
            // kMaxDeclined cuts it. After two such descents in a row the
            // strategy sits out a growing number of draws (see backoff_),
            // then tries again: the state (and so the declines) may have
            // changed.
            if (backoff_ > 0) {
                --backoff_;
                return false;
            }

            Snapshot::capture(before_, solution);
            drawn_ruin_ = -1;
            if (!polished_) {
                polished_ = true;
                local_search_.mark_all(solution);
                entities.clear();
            } else {
                // Other operators and rejected iterations change lists the
                // descent never saw: every list that differs from the state
                // the previous iteration left behind is marked again.
                Snapshot::for_each_changed_list(solution, after_,
                                             [&](int l) { local_search_.mark_list(solution, l); });
                const int span = max_ruin_ - min_ruin_ + 1;
                const int ruin_count = std::clamp(
                        min_ruin_ + random_index(rng, span), 1,
                        n - 1);
                drawn_ruin_ = ruin_selector_.select(rng);
                std::vector<int> ruined = ruins_[drawn_ruin_]->select_ruin_set(ctx, ruin_count, rng);
                repair_.dedupe(editor, ruined, n);
                if (ruined.empty()) {
                    // Nothing to do, but the lists marked above were seen:
                    // record the state so the next iteration marks only what
                    // changes from here on.
                    Snapshot::capture(after_, solution);
                    return false;
                }
                for (int e : ruined) local_search_.mark_list(solution, index_.list_of(e));
                repair_.ruin(editor, ruined);
                repair_.recreate(editor, ruined, rng);
                for (int e : ruined) local_search_.mark_list(solution, index_.list_of(e));
            }
            local_search_.run(editor, rng, exclude_list_, max_evaluations_, ctx.stop_requested);
            if (local_search_.declined() >= GranularLocalSearch<Solution>::kMaxDeclined) {
                if (++slow_descents_ >= kMaxSlowDescents) {
                    slow_descents_ = 0;
                    backoff_ = backoff_length_;
                    backoff_length_ = std::min(backoff_length_ * 2, kMaxBackoffDraws);
                }
            } else {
                slow_descents_ = 0;
                backoff_length_ = kFirstBackoffDraws;
            }
            Snapshot::capture(after_, solution);
            // The move touched every element of every list it changed, for
            // the entity tabu and anything else keyed on the moved elements.
            // Nothing changed (a polish with nothing to do, a repair that put
            // every element back): not a move, and the ruin is not credited.
            entities.clear();
            Snapshot::append_changed_elements(solution, before_, entities);
            if (entities.empty()) {
                drawn_ruin_ = -1;
                return false;
            }
            return true;
        }
    }

    // Restores only the lists that differ from the pre-move snapshot, through
    // the editor's list hooks, so a rejected iteration costs what it
    // touched rather than a full recompute of the solution.
    void undo_last(OperatorContext<Solution>& ctx) override {
        if constexpr (Traits::shape == Shape::Sequence) {
            Editor<Solution>& editor = ctx.editor;
            Solution& solution = editor.solution();
            Snapshot::for_each_changed_list(solution, before_, [&](int l) {
                Snapshot::restore_list(editor, before_, l);
                index_.reindex(solution, l);
                // Back in a state the descent consumed the dirty bits for but
                // never examined: mark it.
                local_search_.mark_list(solution, l);
            });
        } else {
            Snapshot::restore(ctx.editor, before_);
        }
        std::swap(after_, before_);  // the restored state is what the lists look like now
    }

    void outcome(MoveOutcome outcome) override {
        if (drawn_ruin_ >= 0) ruin_selector_.update(drawn_ruin_, outcome);
    }

private:
    ElementIndex<Solution>& index_;
    GreedyRepair<Solution>& repair_;
    GranularLocalSearch<Solution>& local_search_;
    const std::vector<RuinOperator<Solution>*>& ruins_;
    OperatorSelectionScheme& ruin_selector_;
    int min_ruin_;
    int max_ruin_;
    int exclude_list_;
    std::int64_t max_evaluations_;
    // Consecutive descents cut short by kMaxDeclined, the draws still to sit
    // out after kMaxSlowDescents of them, and how long the next sit-out is.
    // The sit-out doubles while the cuts continue (64 draws, 128, ... up to
    // kMaxBackoffDraws, which is on the order of a second of ordinary moves)
    // and resets once a descent completes: an objective that keeps declining
    // ends up spending a small fraction of the solve in cut descents, one
    // whose state changed is retried within a fraction of a second. Not a
    // config knob: the built-in objectives never decline, so nothing tunes it.
    int slow_descents_ = 0;
    int backoff_ = 0;
    int backoff_length_ = kFirstBackoffDraws;
    static constexpr int kMaxSlowDescents = 2;
    static constexpr int kFirstBackoffDraws = 64;
    static constexpr int kMaxBackoffDraws = 1 << 16;
    int drawn_ruin_ = -1;
    bool polished_ = false;
    SolutionSnapshot<Solution> before_;
    SolutionSnapshot<Solution> after_;  // the lists as the last iteration left them
};

}  // namespace optimaru

#endif  // OPTIMARU_MOVES_ITERATED_LOCAL_SEARCH_HPP

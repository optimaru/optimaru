// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_MOVES_ITERATED_LOCAL_SEARCH_HPP
#define OPTIMARU_SEARCH_MOVES_ITERATED_LOCAL_SEARCH_HPP

#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include "optimaru/solution/solution_traits.hpp"
#include "optimaru/solution/neighborhoods.hpp"
#include "optimaru/select/scheme.hpp"
#include "optimaru/solution/element_index.hpp"
#include "optimaru/search/ruin_recreate/greedy_repair.hpp"
#include "optimaru/search/ruin_recreate/removal_gain.hpp"
#include "optimaru/search/ruin_recreate/ruin_count.hpp"
#include "optimaru/search/moves/granular_local_search.hpp"
#include "optimaru/search/moves/list_move.hpp"
#include "optimaru/stop/stop_throttle.hpp"
#include "optimaru/solution/snapshot.hpp"
#include "optimaru/search/operators.hpp"
#include "optimaru/utils/random.hpp"

namespace optimaru {

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
                Snapshot::for_each_changed_list(
                        solution, after_, [&](int l) { local_search_.mark_list(solution, l); });
                const int span = max_ruin_ - min_ruin_ + 1;
                const int ruin_count = clamp_ruin_count(min_ruin_ + random_index(rng, span), n);
                drawn_ruin_ = ruin_selector_.select(rng);
                std::vector<int> ruined =
                        ruins_[drawn_ruin_]->select_ruin_set(ctx, ruin_count, rng);
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

#endif  // OPTIMARU_SEARCH_MOVES_ITERATED_LOCAL_SEARCH_HPP

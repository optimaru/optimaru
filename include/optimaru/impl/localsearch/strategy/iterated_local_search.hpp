#ifndef OPTIMARU_IMPL_LOCALSEARCH_STRATEGY_ITERATED_LOCAL_SEARCH_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_STRATEGY_ITERATED_LOCAL_SEARCH_HPP

#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/impl/heuristic/selector/common/nearby/nearby_neighborhoods.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/select/operator_selection_scheme.hpp"
#include "optimaru/impl/localsearch/element_index.hpp"
#include "optimaru/impl/localsearch/greedy_repair.hpp"
#include "optimaru/impl/localsearch/list_move.hpp"
#include "optimaru/impl/localsearch/termination_throttle.hpp"
#include "optimaru/impl/localsearch/solution_snapshot.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// GranularLocalSearch — first-improvement descent to a local optimum (LIST)
// ---------------------------------------------------------------------------
// The neighbourhood of the HGS / SISR / AILS family of VRP solvers: for every
// element u and every nearby v, the handful of moves that reconnect u next to
// v — relocate (before/after), swap, 2-opt within a list, 2-opt* across lists,
// Or-opt segments and small CROSS exchanges — plus the relocation of u into an
// empty list. The first improving move is applied and the search goes on
// until no element has one left ("don't look bits": an element is only
// re-examined after one of the lists it touches has changed).
//
// Every candidate is scored through the calculator's previews, so one
// evaluation is O(1) on a calculator that implements them, and the whole
// descent is O(elements x nearby) per pass. Without previews it still works
// (apply / score / undo), just slowly.
//
// The excluded list (an unassigned pool) is neither a source nor a target:
// moving elements in or out of it is the repair's and the ejection chain's
// job, where hard feasibility is judged as a whole.
template <typename Solution>
class GranularLocalSearch {
    using Descriptor = SolutionDescriptor<Solution>;

public:
    GranularLocalSearch(ElementIndex<Solution>& index, const NearbyNeighborhoods& nearby,
                        int maxSegment)
        : index_(index),
          nearby_(nearby),
          maxSegment_(std::clamp(maxSegment, 1, ListMove::kMaxSegment)) {}

    // Every element is re-examined on the next run().
    void markAll(const Solution& solution) {
        dirty_.assign(static_cast<std::size_t>(Descriptor::elementCount(solution)), 1);
    }

    // The elements of `list` are re-examined on the next run().
    void markList(const Solution& solution, int list) {
        if (list < 0) return;
        dirty_.resize(static_cast<std::size_t>(Descriptor::elementCount(solution)), 1);
        const int size = Descriptor::listSize(solution, list);
        for (int p = 0; p < size; ++p) {
            dirty_[static_cast<std::size_t>(Descriptor::getElement(solution, list, p))] = 1;
        }
    }

    // Descend until no marked element has an improving move, or the
    // evaluation budget is spent. Returns the number of moves applied.
    // `stopRequested` (may be null) is the solver's termination, consulted
    // through a TerminationThrottle: a preview on a plan with waiting or on a
    // decomposed calculator costs O(route), so the evaluation budget alone
    // would let one descent run seconds past the time limit.
    int run(ScoreDirector<Solution>& director, std::mt19937_64& rng, int excludeList,
            std::int64_t maxEvaluations, const std::function<bool()>* stopRequested = nullptr) {
        if constexpr (Descriptor::kind != VariableKind::LIST) {
            (void)director;
            (void)rng;
            (void)excludeList;
            (void)maxEvaluations;
            (void)stopRequested;
            return 0;
        } else {
            if (!nearby_.enabled()) return 0;
            const Solution& solution = director.workingSolution();
            const int elementCount = Descriptor::elementCount(solution);
            dirty_.resize(static_cast<std::size_t>(elementCount), 1);
            order_.resize(static_cast<std::size_t>(elementCount));
            for (int e = 0; e < elementCount; ++e) order_[static_cast<std::size_t>(e)] = e;
            evaluations_ = 0;
            declined_ = 0;
            excludeList_ = excludeList;
            int applied = 0;
            bool any = true;
            // Declined previews are scored by applying the move, O(list) each;
            // a descent that keeps hitting them stops early so one iteration
            // cannot run far past the time limit, which is only read between
            // steps. The termination itself is asked through the throttle
            // for the same reason.
            throttle_.reset(stopRequested);
            auto exhausted = [&] {
                return throttle_.reached(evaluations_) || evaluations_ >= maxEvaluations ||
                       declined_ >= kMaxDeclined;
            };
            while (any && !exhausted()) {
                any = false;
                std::shuffle(order_.begin(), order_.end(), rng);
                for (int u : order_) {
                    if (exhausted()) break;
                    if (!dirty_[static_cast<std::size_t>(u)]) continue;
                    dirty_[static_cast<std::size_t>(u)] = 0;
                    const int lu = index_.listOf(u);
                    if (lu < 0 || lu == excludeList) continue;
                    if (improveAround(director, u)) {
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
    HardSoftScore evaluate(ScoreDirector<Solution>& director, const ListMove& m) {
        ++evaluations_;
        bool served = false;
        const HardSoftScore score = Moves::evaluate(director, index_, m, &served);
        if (!served) ++declined_;
        return score;
    }

    // The first improving move around u, applied. Both lists it touched are
    // marked so their elements get another look.
    bool improveAround(ScoreDirector<Solution>& director, int u) {
        const Solution& solution = director.workingSolution();
        const HardSoftScore current = director.calculateScore();
        const int lu = index_.listOf(u);
        const int pu = index_.positionOf(u);
        const int su = Descriptor::listSize(solution, lu);

        auto tryMove = [&](const ListMove& m) {
            if (evaluate(director, m) > current) {
                Moves::apply(director, index_, m);
                markList(director.workingSolution(), m.l1);
                markList(director.workingSolution(), m.l2);
                return true;
            }
            return false;
        };

        // Into an empty list: the one move no neighbour of u suggests. Every
        // empty list is tried, since lists need not be interchangeable (a
        // heterogeneous fleet).
        const int listCount = Descriptor::listCount(solution);
        for (int l = 0; l < listCount; ++l) {
            if (l == lu || l == excludeList_ || Descriptor::listSize(solution, l) != 0) continue;
            if (tryMove(ListMove::segment(lu, pu, 1, l, 0))) return true;
        }

        for (int v : nearby_.of(u)) {
            const int lv = index_.listOf(v);
            if (lv < 0 || lv == excludeList_) continue;
            const int pv = index_.positionOf(v);
            const int sv = Descriptor::listSize(solution, lv);
            const bool same = lu == lv;

            // Relocate u after v, and before v.
            {
                const int after = same ? (pv < pu ? pv + 1 : pv) : pv + 1;
                if (!(same && after == pu)) {
                    if (tryMove(ListMove::segment(lu, pu, 1, lv, after))) return true;
                }
                const int before = same ? (pv < pu ? pv : pv - 1) : pv;
                if (!(same && before == pu)) {
                    if (tryMove(ListMove::segment(lu, pu, 1, lv, before))) return true;
                }
            }
            // Swap u and v.
            if (tryMove(ListMove::swap(lu, pu, lv, pv))) return true;

            if (same) {
                // 2-opt: reverse the stretch between u and v, either way round.
                const int a = std::min(pu, pv), b = std::max(pu, pv);
                if (b - a >= 2) {
                    if (tryMove(ListMove::reverse(lu, a + 1, b))) return true;
                    if (tryMove(ListMove::reverse(lu, a, b - 1))) return true;
                }
            } else {
                // 2-opt*: exchange the tails after u and v, or before them.
                if (pu + 1 < su || pv + 1 < sv) {
                    if (tryMove(ListMove::tailSwap(lu, pu + 1, lv, pv + 1))) return true;
                }
                if (tryMove(ListMove::tailSwap(lu, pu, lv, pv))) return true;
                // CROSS: the pair starting at u against v, or v's pair.
                if (pu + 1 < su) {
                    if (tryMove(ListMove::exchange(lu, pu, 2, lv, pv, 1))) return true;
                    if (pv + 1 < sv) {
                        if (tryMove(ListMove::exchange(lu, pu, 2, lv, pv, 2))) return true;
                    }
                }
                if (pv + 1 < sv) {
                    if (tryMove(ListMove::exchange(lu, pu, 1, lv, pv, 2))) return true;
                }
            }
            // Or-opt: the segment starting at u, relocated after v.
            for (int len = 2; len <= maxSegment_ && pu + len <= su; ++len) {
                if (same && pv >= pu && pv < pu + len) continue;  // v inside the segment
                const int at = same ? (pv < pu ? pv + 1 : pv + 1 - len) : pv + 1;
                if (same && at == pu) continue;
                if (tryMove(ListMove::segment(lu, pu, len, lv, at))) return true;
            }
        }
        return false;
    }

    ElementIndex<Solution>& index_;
    const NearbyNeighborhoods& nearby_;
    int maxSegment_;
    int excludeList_ = -1;
    std::int64_t evaluations_ = 0;
    TerminationThrottle throttle_;
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
// The operator declines on a calculator whose supportsPreviews() is false
// (the default implementation): the same descent through apply/score/undo
// would take seconds per iteration for nothing the ordinary moves cannot do.
// One that previews in general but declines most moves for the plan at hand
// is backed off from for a while after two descents in a row were cut by
// GranularLocalSearch::kMaxDeclined.
template <typename Solution>
class IteratedLocalSearchMove final : public MoveOperator<Solution> {
    using Descriptor = SolutionDescriptor<Solution>;
    using Snapshot = SolutionSnapshot<Solution>;

public:
    IteratedLocalSearchMove(ElementIndex<Solution>& index, GreedyRepair<Solution>& repair,
                            GranularLocalSearch<Solution>& localSearch,
                            const std::vector<RuinOperator<Solution>*>& ruins,
                            OperatorSelectionScheme& ruinSelector, int minRuin, int maxRuin,
                            int excludeList, std::int64_t maxEvaluations)
        : index_(index),
          repair_(repair),
          localSearch_(localSearch),
          ruins_(ruins),
          ruinSelector_(ruinSelector),
          minRuin_(std::max(1, minRuin)),
          maxRuin_(std::max(minRuin_, maxRuin)),
          excludeList_(excludeList),
          maxEvaluations_(maxEvaluations) {}

    const char* name() const override { return "iteratedLocalSearch"; }

    bool apply(OperatorContext<Solution>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        if constexpr (Descriptor::kind != VariableKind::LIST) {
            (void)ctx;
            (void)rng;
            (void)entities;
            return false;
        } else {
            ScoreDirector<Solution>& director = ctx.director;
            Solution& solution = director.workingSolution();
            const int n = GreedyRepair<Solution>::candidateCount(solution);
            if (n < 2 || ruins_.empty() || !ctx.nearby.enabled()) return false;
            if (!director.calculator().supportsPreviews()) return false;
            // A calculator that says it previews but declines most moves for
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
            drawnRuin_ = -1;
            if (!polished_) {
                polished_ = true;
                localSearch_.markAll(solution);
                entities.clear();
            } else {
                // Other operators and rejected iterations change lists the
                // descent never saw: every list that differs from the state
                // the previous iteration left behind is marked again.
                Snapshot::forEachChangedList(solution, after_,
                                             [&](int l) { localSearch_.markList(solution, l); });
                const int span = maxRuin_ - minRuin_ + 1;
                const int ruinCount = std::clamp(
                        minRuin_ + static_cast<int>(rng() % static_cast<std::uint64_t>(span)), 1,
                        n - 1);
                drawnRuin_ = ruinSelector_.select(rng);
                std::vector<int> ruined = ruins_[drawnRuin_]->selectRuinSet(ctx, ruinCount, rng);
                repair_.dedupe(director, ruined, n);
                if (ruined.empty()) {
                    // Nothing to do, but the lists marked above were seen:
                    // record the state so the next iteration marks only what
                    // changes from here on.
                    Snapshot::capture(after_, solution);
                    return false;
                }
                for (int e : ruined) localSearch_.markList(solution, index_.listOf(e));
                repair_.ruin(director, ruined);
                repair_.recreate(director, ruined, rng);
                for (int e : ruined) localSearch_.markList(solution, index_.listOf(e));
            }
            localSearch_.run(director, rng, excludeList_, maxEvaluations_, ctx.stopRequested);
            if (localSearch_.declined() >= GranularLocalSearch<Solution>::kMaxDeclined) {
                if (++slowDescents_ >= kMaxSlowDescents) {
                    slowDescents_ = 0;
                    backoff_ = backoffLength_;
                    backoffLength_ = std::min(backoffLength_ * 2, kMaxBackoffDraws);
                }
            } else {
                slowDescents_ = 0;
                backoffLength_ = kFirstBackoffDraws;
            }
            Snapshot::capture(after_, solution);
            // The move touched every element of every list it changed, for
            // the entity tabu and anything else keyed on the moved elements.
            // Nothing changed (a polish with nothing to do, a repair that put
            // every element back): not a move, and the ruin is not credited.
            entities.clear();
            Snapshot::appendChangedElements(solution, before_, entities);
            if (entities.empty()) {
                drawnRuin_ = -1;
                return false;
            }
            return true;
        }
    }

    // Restores only the lists that differ from the pre-move snapshot, through
    // the director's list hooks, so a rejected iteration costs what it
    // touched rather than a full recompute of the solution.
    void undoLast(OperatorContext<Solution>& ctx) override {
        if constexpr (Descriptor::kind == VariableKind::LIST) {
            ScoreDirector<Solution>& director = ctx.director;
            Solution& solution = director.workingSolution();
            Snapshot::forEachChangedList(solution, before_, [&](int l) {
                Snapshot::restoreList(director, before_, l);
                index_.reindex(solution, l);
                // Back in a state the descent consumed the dirty bits for but
                // never examined: mark it.
                localSearch_.markList(solution, l);
            });
        } else {
            Snapshot::restore(ctx.director, before_);
        }
        std::swap(after_, before_);  // the restored state is what the lists look like now
    }

    void outcome(LnsOutcome outcome) override {
        if (drawnRuin_ >= 0) ruinSelector_.update(drawnRuin_, outcome);
    }

private:
    ElementIndex<Solution>& index_;
    GreedyRepair<Solution>& repair_;
    GranularLocalSearch<Solution>& localSearch_;
    const std::vector<RuinOperator<Solution>*>& ruins_;
    OperatorSelectionScheme& ruinSelector_;
    int minRuin_;
    int maxRuin_;
    int excludeList_;
    std::int64_t maxEvaluations_;
    // Consecutive descents cut short by kMaxDeclined, the draws still to sit
    // out after kMaxSlowDescents of them, and how long the next sit-out is.
    // The sit-out doubles while the cuts continue (64 draws, 128, ... up to
    // kMaxBackoffDraws, which is on the order of a second of ordinary moves)
    // and resets once a descent completes: a calculator that keeps declining
    // ends up spending a small fraction of the solve in cut descents, one
    // whose state changed is retried within a fraction of a second. Not a
    // config knob: the built-in calculators never decline, so nothing tunes it.
    int slowDescents_ = 0;
    int backoff_ = 0;
    int backoffLength_ = kFirstBackoffDraws;
    static constexpr int kMaxSlowDescents = 2;
    static constexpr int kFirstBackoffDraws = 64;
    static constexpr int kMaxBackoffDraws = 1 << 16;
    int drawnRuin_ = -1;
    bool polished_ = false;
    SolutionSnapshot<Solution> before_;
    SolutionSnapshot<Solution> after_;  // the lists as the last iteration left them
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_STRATEGY_ITERATED_LOCAL_SEARCH_HPP

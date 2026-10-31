#ifndef OPTIMARU_IMPL_LOCALSEARCH_STRATEGY_EJECTION_CHAIN_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_STRATEGY_EJECTION_CHAIN_HPP

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <random>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/impl/heuristic/selector/common/nearby/nearby_neighborhoods.hpp"
#include "optimaru/impl/localsearch/element_index.hpp"
#include "optimaru/impl/localsearch/list_move.hpp"
#include "optimaru/impl/localsearch/termination_throttle.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Ejection chain — break a hard constraint on purpose, then repair it
// ---------------------------------------------------------------------------
// On a tightly packed instance the hard-feasible region is a set of islands.
// CVRPLIB X-n895-k37 loads its 37 vehicles to 99.8% with demands of 50-100:
// the last visit fits nowhere, and the only way to make room is to shift
// slack between routes through a chain of swaps, every one of which is
// uphill in soft score and none of which pays off until the last. Late
// acceptance never takes a hard step back, so the chain is unreachable one
// move at a time.
//
// This operator applies the whole chain as one move:
//
//  1. Seed. With a configured source list (the unassigned pool) that is not
//     empty, half the time take one of its elements and insert it at its
//     best position in any other list, even if that breaks a hard
//     constraint. Otherwise relocate a random element to the nearby
//     position with the best soft score among those that break a hard
//     constraint — the move the feasibility wall was hiding.
//     The repair may never put the seeded element back where it started:
//     that is a free full restoration and would turn the chain into a no-op.
//  2. Repair. While the hard score is below where it started, apply the best
//     strictly hard-improving relocate or swap out of the lists the chain last
//     touched, into the lists of each element's nearby neighbours. A swap
//     between an overloaded and a slack list moves exactly the slack the
//     single-move search could not reach.
//  3. If the hard score is back, the acceptor judges the net result. If the
//     chain stalls or hits its step cap, everything is undone and the move
//     reports itself as not applied. So does a chain that exhausts its score
//     evaluation budget: the step cap alone does not bound the cost, since
//     one step scans every element of up to two lists against every
//     candidate slot and swap partner.
//
// The repair never moves anything into the source list: "unassign something
// else" would be a lexicographic improvement at every step and would turn the
// chain into a trade that leaves the pool exactly as large as before.
//
// LIST kind only; apply() declines on BASIC.
template <typename Solution>
class EjectionChain final : public MoveOperator<Solution> {
    using Descriptor = SolutionDescriptor<Solution>;

public:
    EjectionChain(ElementIndex<Solution>& index, const NearbyNeighborhoods& nearby,
                  int sourceList, int maxSteps, int candidateLists, std::int64_t maxEvaluations)
        : index_(index),
          nearby_(nearby),
          sourceList_(sourceList),
          maxSteps_(std::max(1, maxSteps)),
          candidateLists_(std::max(1, candidateLists)),
          maxEvaluations_(std::max<std::int64_t>(1, maxEvaluations)) {}

    const char* name() const override { return "ejectionChain"; }

    bool apply(OperatorContext<Solution>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        applied_.clear();
        if constexpr (Descriptor::kind != VariableKind::LIST) {
            (void)ctx;
            (void)rng;
            (void)entities;
            return false;
        } else {
            ScoreDirector<Solution>& director = ctx.director;
            const Solution& solution = director.workingSolution();
            if (Descriptor::listCount(solution) < 2 || !nearby_.enabled()) return false;

            const HardSoftScore start = director.calculateScore();
            evaluations_ = 0;
            // Half the chains start from the pool while it is non-empty: an
            // element that can never be placed (an impossible time window)
            // would otherwise sink every attempt into a failing repair and
            // starve the slack-shifting chains that start from routed
            // elements.
            const bool fromSource =
                    hasSource(solution) && std::uniform_int_distribution<int>(0, 1)(rng) == 0;
            const bool seeded = fromSource ? seedFromSource(director, rng)
                                           : seedInfeasibleRelocate(director, rng, start);
            if (!seeded) return false;
            seedElement_ = applied_.front().element1;
            seedOrigin_ = applied_.front().l1;

            HardSoftScore current = director.calculateScore();
            // The repair asks the termination between steps once enough
            // evaluations have accumulated: on a calculator whose previews
            // cost O(route) the evaluation budget alone is seconds.
            throttle_.reset(ctx.stopRequested);
            for (int step = 0; step < maxSteps_ && current.hard < start.hard &&
                               evaluations_ < maxEvaluations_ && !throttle_.reached(evaluations_);
                 ++step) {
                if (!bestRepair(director, current)) break;
                current = director.calculateScore();
            }
            if (current.hard < start.hard) {
                undoAll(director);
                return false;
            }

            entities.clear();
            for (const ListMove& op : applied_) {
                entities.push_back(op.element1);
                if (op.kind == ListMove::Kind::SWAP) entities.push_back(op.element2);
            }
            return true;
        }
    }

    void undoLast(OperatorContext<Solution>& ctx) override {
        if constexpr (Descriptor::kind != VariableKind::LIST) {
            (void)ctx;
        } else {
            undoAll(ctx.director);
        }
    }

private:
    using Moves = ListMoves<Solution>;

    struct Slot {
        int list;
        int position;
    };

    // A chain's operations are ListMoves of two kinds: a SEGMENT of length 1
    // (element1 moved from (l1, p1) to (l2, p2)) and a SWAP (element1 at
    // (l1, p1) and element2 at (l2, p2) traded places).

    bool hasSource(const Solution& solution) const {
        return sourceList_ >= 0 && sourceList_ < Descriptor::listCount(solution) &&
               Descriptor::listSize(solution, sourceList_) > 0;
    }

    void undoAll(ScoreDirector<Solution>& director) {
        for (auto it = applied_.rbegin(); it != applied_.rend(); ++it) {
            Moves::undo(director, index_, *it);
        }
        applied_.clear();
    }

    // Score of the solution with `op` applied (see ListMoves::evaluate).
    // Relocations always cross lists here, so `p2` indexes the target list
    // as it is now, which is also what the preview contract expects.
    HardSoftScore evaluate(ScoreDirector<Solution>& director, const ListMove& op) {
        return Moves::evaluate(director, index_, op);
    }

    // Relocation targets for `e`: the slots beside its nearby neighbours in
    // other lists, plus each such list's end so a near-empty list is still
    // reachable. `from` and the source list are never targets.
    void collectRelocations(const Solution& solution, int e, int from) {
        relocations_.clear();
        lists_.clear();
        for (int nb : nearby_.of(e)) {
            const int list = index_.listOf(nb);
            if (list < 0 || list == from || list == sourceList_) continue;
            const bool known = std::find(lists_.begin(), lists_.end(), list) != lists_.end();
            if (!known) {
                if (static_cast<int>(lists_.size()) >= candidateLists_) continue;
                lists_.push_back(list);
            }
            addSlot({list, index_.positionOf(nb)});
            addSlot({list, index_.positionOf(nb) + 1});
        }
        for (int list : lists_) addSlot({list, Descriptor::listSize(solution, list)});
    }

    // Neighbours that sit next to each other in one list share a slot; each
    // slot is scored once so the evaluation budget goes to distinct moves.
    void addSlot(const Slot& slot) {
        for (const Slot& known : relocations_) {
            if (known.list == slot.list && known.position == slot.position) return;
        }
        relocations_.push_back(slot);
    }

    bool seedFromSource(ScoreDirector<Solution>& director, std::mt19937_64& rng) {
        const Solution& solution = director.workingSolution();
        const int size = Descriptor::listSize(solution, sourceList_);
        const int p1 = static_cast<int>(rng() % static_cast<std::uint64_t>(size));
        const int e = Descriptor::getElement(solution, sourceList_, p1);

        bool found = false;
        ListMove best{};
        HardSoftScore bestScore;
        // Targets never include the source list, so their positions are the
        // same whether or not `e` has been taken out yet.
        collectRelocations(solution, e, sourceList_);
        // A source element whose neighbours are all unassigned still needs a
        // landing spot: every list's end.
        if (relocations_.empty()) {
            for (int l = 0; l < Descriptor::listCount(solution); ++l) {
                if (l != sourceList_) relocations_.push_back({l, Descriptor::listSize(solution, l)});
            }
        }
        for (const Slot& slot : relocations_) {
            if (slot.position > Descriptor::listSize(solution, slot.list)) continue;
            const ListMove op = ListMove::segment(sourceList_, p1, 1, slot.list, slot.position, e);
            const HardSoftScore score = evaluate(director, op);
            ++evaluations_;
            if (!found || score > bestScore) {
                found = true;
                bestScore = score;
                best = op;
            }
        }
        if (!found) return false;
        Moves::apply(director, index_, best);
        applied_.push_back(best);
        return true;
    }

    // The best-soft relocation of a random element among those that break a
    // hard constraint and would improve soft score — the move the
    // feasibility wall hides. Declines when there is none.
    bool seedInfeasibleRelocate(ScoreDirector<Solution>& director, std::mt19937_64& rng,
                                const HardSoftScore& start) {
        const Solution& solution = director.workingSolution();
        const int elementCount = Descriptor::elementCount(solution);
        if (elementCount < 2) return false;
        // A few draws to land on a routed element: with a large pool, one
        // draw that hits it would throw the whole step away.
        int e = -1;
        int from = -1;
        for (int draw = 0; draw < 8 && (from < 0 || from == sourceList_); ++draw) {
            e = static_cast<int>(rng() % static_cast<std::uint64_t>(elementCount));
            from = index_.listOf(e);
        }
        if (from < 0 || from == sourceList_) return false;
        const int p1 = index_.positionOf(e);

        bool found = false;
        ListMove best{};
        double bestSoft = start.soft;
        collectRelocations(solution, e, from);
        for (const Slot& slot : relocations_) {
            if (slot.position > Descriptor::listSize(solution, slot.list)) continue;
            const ListMove op = ListMove::segment(from, p1, 1, slot.list, slot.position, e);
            const HardSoftScore score = evaluate(director, op);
            ++evaluations_;
            if (score.hard < start.hard && score.soft > bestSoft) {
                found = true;
                bestSoft = score.soft;
                best = op;
            }
        }
        if (!found) return false;
        Moves::apply(director, index_, best);
        applied_.push_back(best);
        return true;
    }

    // One repair step: the best strictly hard-improving relocate or swap out
    // of the lists the previous operation touched.
    bool bestRepair(ScoreDirector<Solution>& director, const HardSoftScore& current) {
        const Solution& solution = director.workingSolution();
        const ListMove& last = applied_.back();
        active_.clear();
        active_.push_back(last.l2);
        if (last.l1 != last.l2 && last.l1 != sourceList_) active_.push_back(last.l1);

        bool found = false;
        ListMove best{};
        HardSoftScore bestScore = current;
        auto consider = [&](const ListMove& op) {
            if (evaluations_ >= maxEvaluations_ || returnsSeed(op)) return;
            // The repair never feeds the source list (see the header): every
            // target comes from collectRelocations / lists_, which skip it.
            assert(op.l2 != sourceList_);
            ++evaluations_;
            const HardSoftScore score = evaluate(director, op);
            if (score.hard > current.hard && (!found || score > bestScore)) {
                found = true;
                bestScore = score;
                best = op;
            }
        };

        for (int a : active_) {
            for (int p1 = 0; p1 < Descriptor::listSize(solution, a); ++p1) {
                // Out of budget: the chain will be undone, stop scanning.
                if (evaluations_ >= maxEvaluations_) break;
                const int e = Descriptor::getElement(solution, a, p1);
                collectRelocations(solution, e, a);
                // Relocation positions are measured with e still in place;
                // removing it first shifts nothing in another list.
                for (const Slot& slot : relocations_) {
                    consider(ListMove::segment(a, p1, 1, slot.list, slot.position, e));
                }
                // Swap partners: every element of the candidate lists while
                // that set is no larger than the nearby size, otherwise just
                // e's nearby neighbours on them. Either way one step stays
                // O(L x nearby); the full scan on 25-stop routes is what let
                // a chain burn its whole budget (CVRPLIB X-n895-k37), while
                // on 5-stop routes (X-n289-k60) the partner outside the
                // neighbourhood is the one that carries the slack.
                int partnerCount = 0;
                for (int list : lists_) partnerCount += Descriptor::listSize(solution, list);
                if (partnerCount <= static_cast<int>(nearby_.of(e).size())) {
                    for (int list : lists_) {
                        for (int p2 = 0; p2 < Descriptor::listSize(solution, list); ++p2) {
                            if (evaluations_ >= maxEvaluations_) break;
                            consider(ListMove::swap(a, p1, list, p2, e,
                                                    Descriptor::getElement(solution, list, p2)));
                        }
                    }
                } else {
                    for (int nb : nearby_.of(e)) {
                        if (evaluations_ >= maxEvaluations_) break;
                        const int list = index_.listOf(nb);
                        if (std::find(lists_.begin(), lists_.end(), list) == lists_.end())
                            continue;
                        consider(ListMove::swap(a, p1, list, index_.positionOf(nb), e, nb));
                    }
                }
            }
        }
        if (!found) return false;
        Moves::apply(director, index_, best);
        applied_.push_back(best);
        return true;
    }

    // Would `op` put the seeded element back on the list it started from?
    // That restores the starting hard score exactly at no soft cost, so it
    // would outbid every partial repair and collapse the chain into a no-op
    // the acceptor then spends a step on.
    bool returnsSeed(const ListMove& op) const {
        if (op.element1 == seedElement_ && op.l2 == seedOrigin_) return true;
        return op.kind == ListMove::Kind::SWAP && op.element2 == seedElement_ &&
               op.l1 == seedOrigin_;
    }

    ElementIndex<Solution>& index_;
    const NearbyNeighborhoods& nearby_;
    int sourceList_;
    int maxSteps_;
    int candidateLists_;
    std::int64_t maxEvaluations_;
    std::int64_t evaluations_ = 0;
    TerminationThrottle throttle_;
    int seedElement_ = -1;
    int seedOrigin_ = -1;

    std::vector<ListMove> applied_;
    std::vector<Slot> relocations_;
    std::vector<int> lists_;
    std::vector<int> active_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_STRATEGY_EJECTION_CHAIN_HPP

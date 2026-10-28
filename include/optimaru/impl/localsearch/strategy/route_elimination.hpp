#ifndef OPTIMARU_IMPL_LOCALSEARCH_STRATEGY_ROUTE_ELIMINATION_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_STRATEGY_ROUTE_ELIMINATION_HPP

#include <algorithm>
#include <cstddef>
#include <random>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/impl/localsearch/element_index.hpp"
#include "optimaru/impl/localsearch/greedy_repair.hpp"
#include "optimaru/impl/localsearch/solution_snapshot.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Route elimination — empty one whole list, atomically
// ---------------------------------------------------------------------------
// Takes every element off the smallest non-empty list and cheapest-inserts it
// into some OTHER list, as a single move the acceptor judges as a whole.
//
// Why the (1,0) relocate moves cannot do this on their own, however well they
// are searched: emptying a list of k elements takes k relocations, and the
// payoff — dropping the list's two depot legs — arrives only when the LAST
// element leaves. Every intermediate step is uphill, a detour added to some
// other route while the emptying route still pays its full cost. A
// best-improvement descent can never take the first step, and late acceptance
// would have to draw exactly the right k relocations in sequence. Applying the
// whole thing at once is what lets the acceptor see the true net effect
// instead of the worst intermediate.
//
// It targets the SMALLEST list on purpose: the one most likely to be
// absorbable by its neighbours, and the one whose removal is nearly free.
//
// This is, structurally, the route ruin followed by a repair forbidden from
// using the route it just emptied. That exclusion is the whole trick — without
// it the repair simply puts everything back where it came from, which is why
// the existing route ruin could never actually retire a route.
//
// LIST kind only; apply() declines on BASIC.
template <typename Solution>
class RouteElimination final : public MoveOperator<Solution> {
    using Descriptor = SolutionDescriptor<Solution>;

public:
    RouteElimination(ElementIndex<Solution>& index, GreedyRepair<Solution>& repair, int maxSize)
        : index_(index), repair_(repair), maxSize_(maxSize) {}

    const char* name() const override { return "routeElimination"; }

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
            const int listCount = Descriptor::listCount(solution);
            if (listCount < 2) return false;

            // Smallest non-empty list, and only if it is small enough to be
            // worth trying: past a certain size the reinsertion is both
            // expensive and hopeless.
            // The repair's excluded list (an unassigned pool) is not a route:
            // never the target, and never where the displaced elements go —
            // so there has to be some other list left to take them.
            const int excluded = repair_.excludedList();
            int target = -1;
            int smallest = maxSize_ + 1;
            int others = 0;
            for (int l = 0; l < listCount; ++l) {
                if (l == excluded) continue;
                ++others;
                const int size = Descriptor::listSize(solution, l);
                if (size > 0 && size < smallest) {
                    smallest = size;
                    target = l;
                }
            }
            if (target < 0 || others < 2) return false;

            SolutionSnapshot<Solution>::capture(before_, solution);
            displaced_.clear();
            displaced_.reserve(static_cast<std::size_t>(smallest));
            while (Descriptor::listSize(solution, target) > 0) {
                displaced_.push_back(index_.removeAt(director, target, 0));
            }

            // Reinsert one at a time, in random order, everywhere BUT the list
            // being emptied.
            std::shuffle(displaced_.begin(), displaced_.end(), rng);
            for (int element : displaced_) {
                repair_.insertCheapest(director, element, rng, /*excludeList=*/target,
                                       repair_.excludedList());
            }

            entities = displaced_;
            return true;
        }
    }

    void undoLast(OperatorContext<Solution>& ctx) override {
        if constexpr (Descriptor::kind != VariableKind::LIST) {
            (void)ctx;
        } else {
            // The repair scattered the elements across every other list, so
            // there is no cheap inverse to replay: restore wholesale. The
            // snapshot write bypasses the director's list hooks, so the index
            // has to be rebuilt rather than reindexed per touched list.
            SolutionSnapshot<Solution>::restore(ctx.director, before_);
            index_.build(ctx.director.workingSolution());
        }
    }

private:
    ElementIndex<Solution>& index_;
    GreedyRepair<Solution>& repair_;
    int maxSize_;
    SolutionSnapshot<Solution> before_;
    std::vector<int> displaced_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_STRATEGY_ROUTE_ELIMINATION_HPP

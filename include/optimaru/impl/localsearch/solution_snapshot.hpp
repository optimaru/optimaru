#ifndef OPTIMARU_IMPL_LOCALSEARCH_SOLUTION_SNAPSHOT_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_SOLUTION_SNAPSHOT_HPP

#include <stdexcept>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/director/score_director.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// SolutionSnapshot
// ---------------------------------------------------------------------------
// A plain copy of every genuine variable, used for the two jobs where an
// inverse move is not available: remembering the best solution found, and
// rolling back a compound move (a destroy-and-repair, a whole route emptied)
// whose apply() is far too tangled to invert step by step.
//
// restore() writes the variables back through the ScoreDirector for BASIC, but
// rebuilds LIST solutions directly and then calls resetWorkingSolution() — one
// full recompute instead of one hook pair per element, which is the cheaper
// side of the trade once a compound move has touched most of the lists.
//
// Note for callers on the LIST side: restore() bypasses the director's list
// hooks, so ScoreDirector::touchedLists() does NOT see it and an ElementIndex
// must be rebuilt with build(), not reindexed per list. A caller that knows
// only a few lists changed uses the list-wise helpers instead: sameList(),
// forEachChangedList() and restoreList(), the last of which goes through the
// director's list hooks so the calculator and an ElementIndex::reindex() pay
// for the lists restored, not for the whole solution.
template <typename Solution>
struct SolutionSnapshot {
    using Descriptor = SolutionDescriptor<Solution>;

    std::vector<int> variables;           // BASIC
    std::vector<std::vector<int>> lists;  // LIST

    static SolutionSnapshot take(const Solution& solution) {
        SolutionSnapshot snap;
        capture(snap, solution);
        return snap;
    }

    // take() into existing storage: a caller that snapshots on every move
    // keeps its vectors and pays no allocation once they have grown.
    static void capture(SolutionSnapshot& snap, const Solution& solution) {
        if constexpr (Descriptor::kind == VariableKind::BASIC) {
            const int entityCount = Descriptor::entityCount(solution);
            snap.variables.resize(entityCount);
            for (int e = 0; e < entityCount; ++e) {
                snap.variables[e] = Descriptor::getVariable(solution, e);
            }
        } else {
            const int listCount = Descriptor::listCount(solution);
            snap.lists.resize(listCount);
            for (int l = 0; l < listCount; ++l) {
                const int size = Descriptor::listSize(solution, l);
                snap.lists[l].resize(size);
                for (int p = 0; p < size; ++p) {
                    snap.lists[l][p] = Descriptor::getElement(solution, l, p);
                }
            }
        }
    }

    // LIST kind: whether list `l` currently holds exactly what the snapshot
    // recorded for it. A list the snapshot never saw counts as changed.
    static bool sameList(const Solution& solution, const SolutionSnapshot& snap, int l) {
        if constexpr (Descriptor::kind != VariableKind::LIST) {
            (void)solution; (void)snap; (void)l;
            return true;
        } else {
            if (l < 0 || l >= static_cast<int>(snap.lists.size()) ||
                l >= Descriptor::listCount(solution)) {
                return false;
            }
            const int size = Descriptor::listSize(solution, l);
            if (static_cast<int>(snap.lists[static_cast<std::size_t>(l)].size()) != size) {
                return false;
            }
            const std::vector<int>& recorded = snap.lists[static_cast<std::size_t>(l)];
            for (int p = 0; p < size; ++p) {
                if (recorded[static_cast<std::size_t>(p)] != Descriptor::getElement(solution, l, p)) {
                    return false;
                }
            }
            return true;
        }
    }

    // LIST kind: visit(l) for every list that differs from the snapshot.
    template <typename Visit>
    static void forEachChangedList(const Solution& solution, const SolutionSnapshot& snap,
                                   Visit&& visit) {
        if constexpr (Descriptor::kind == VariableKind::LIST) {
            const int listCount = Descriptor::listCount(solution);
            for (int l = 0; l < listCount; ++l) {
                if (!sameList(solution, snap, l)) visit(l);
            }
        } else {
            (void)solution; (void)snap; (void)visit;
        }
    }

    // LIST kind: appends every element of every list that differs from the
    // snapshot (the elements a compound move touched, for the entity tabu).
    static void appendChangedElements(const Solution& solution, const SolutionSnapshot& snap,
                                      std::vector<int>& out) {
        forEachChangedList(solution, snap, [&](int l) {
            if constexpr (Descriptor::kind == VariableKind::LIST) {
                const int size = Descriptor::listSize(solution, l);
                for (int p = 0; p < size; ++p) out.push_back(Descriptor::getElement(solution, l, p));
            }
        });
    }

    // LIST kind: rewrite list `l` to what the snapshot recorded, through the
    // director's list hooks (one beginListChange/endListChange pair), so the
    // calculator re-costs that list only. The caller reindexes its
    // ElementIndex for `l`.
    static void restoreList(ScoreDirector<Solution>& director, const SolutionSnapshot& snap,
                            int l) {
        if constexpr (Descriptor::kind == VariableKind::LIST) {
            // A list the snapshot never recorded cannot be restored: sameList()
            // reports it as changed, and a caller restoring it has a snapshot
            // of another solution. Loud in every build.
            if (l < 0 || l >= static_cast<int>(snap.lists.size())) {
                throw std::logic_error("SolutionSnapshot::restoreList: list not recorded");
            }
            Solution& solution = director.workingSolution();
            const std::vector<int>& wanted = snap.lists[static_cast<std::size_t>(l)];
            director.beginListChange(l);
            while (Descriptor::listSize(solution, l) > 0) {
                Descriptor::removeElement(solution, l, Descriptor::listSize(solution, l) - 1);
            }
            for (std::size_t p = 0; p < wanted.size(); ++p) {
                Descriptor::insertElement(solution, l, static_cast<int>(p), wanted[p]);
            }
            director.endListChange(l);
        } else {
            (void)director; (void)snap; (void)l;
        }
    }

    static void restore(ScoreDirector<Solution>& director, const SolutionSnapshot& snap) {
        Solution& solution = director.workingSolution();
        if constexpr (Descriptor::kind == VariableKind::BASIC) {
            director.beginVariableChanges();
            for (int e = 0; e < static_cast<int>(snap.variables.size()); ++e) {
                if (Descriptor::getVariable(solution, e) != snap.variables[e]) {
                    director.changeVariable(e, snap.variables[e]);
                }
            }
            director.endVariableChanges();
        } else {
            // One-time rebuild: write the lists directly, then resynchronize
            // the shadow listeners and the calculator with a reset.
            for (int l = 0; l < static_cast<int>(snap.lists.size()); ++l) {
                while (Descriptor::listSize(solution, l) > 0) {
                    Descriptor::removeElement(solution, l, Descriptor::listSize(solution, l) - 1);
                }
                for (int p = 0; p < static_cast<int>(snap.lists[l].size()); ++p) {
                    Descriptor::insertElement(solution, l, p, snap.lists[l][p]);
                }
            }
            director.resetWorkingSolution();
        }
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_SOLUTION_SNAPSHOT_HPP

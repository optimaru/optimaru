// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_SNAPSHOT_HPP
#define OPTIMARU_SEARCH_SNAPSHOT_HPP

#include <stdexcept>
#include <vector>

#include "optimaru/solution_traits.hpp"
#include "optimaru/editor.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------
// A plain copy of every genuine variable, used for the two jobs where an
// inverse move is not available: remembering the best solution found, and
// rolling back a compound move (a destroy-and-repair, a whole route emptied)
// whose apply() is far too tangled to invert step by step.
//
// restore() writes the variables back through the Editor for assignment, but
// rebuilds sequence solutions directly and then calls rebuild() — one
// full recompute instead of one hook pair per element, which is the cheaper
// side of the trade once a compound move has touched most of the lists.
//
// Note for callers on the sequence side: restore() bypasses the editor's list
// hooks, so Editor::touched_lists() does NOT see it and an ElementIndex
// must be rebuilt with build(), not reindexed per list. A caller that knows
// only a few lists changed uses the list-wise helpers instead: same_list(),
// for_each_changed_list() and restore_list(), the last of which goes through the
// editor's list hooks so the objective and an ElementIndex::reindex() pay
// for the lists restored, not for the whole solution.
template <typename Solution>
struct SolutionSnapshot {
    using Traits = SolutionTraits<Solution>;

    std::vector<int> variables;           // assignment
    std::vector<std::vector<int>> lists;  // sequence

    static SolutionSnapshot take(const Solution& solution) {
        SolutionSnapshot snap;
        capture(snap, solution);
        return snap;
    }

    // take() into existing storage: a caller that snapshots on every move
    // keeps its vectors and pays no allocation once they have grown.
    static void capture(SolutionSnapshot& snap, const Solution& solution) {
        if constexpr (Traits::shape == Shape::Assignment) {
            const int entity_count = Traits::entity_count(solution);
            snap.variables.resize(entity_count);
            for (int e = 0; e < entity_count; ++e) {
                snap.variables[e] = Traits::value_of(solution, e);
            }
        } else {
            const int list_count = Traits::list_count(solution);
            snap.lists.resize(list_count);
            for (int l = 0; l < list_count; ++l) {
                const int size = Traits::list_size(solution, l);
                snap.lists[l].resize(size);
                for (int p = 0; p < size; ++p) {
                    snap.lists[l][p] = Traits::element_at(solution, l, p);
                }
            }
        }
    }

    // sequence shape: whether list `l` currently holds exactly what the snapshot
    // recorded for it. A list the snapshot never saw counts as changed.
    static bool same_list(const Solution& solution, const SolutionSnapshot& snap, int l) {
        if constexpr (Traits::shape != Shape::Sequence) {
            (void)solution; (void)snap; (void)l;
            return true;
        } else {
            if (l < 0 || l >= static_cast<int>(snap.lists.size()) ||
                l >= Traits::list_count(solution)) {
                return false;
            }
            const int size = Traits::list_size(solution, l);
            if (static_cast<int>(snap.lists[static_cast<std::size_t>(l)].size()) != size) {
                return false;
            }
            const std::vector<int>& recorded = snap.lists[static_cast<std::size_t>(l)];
            for (int p = 0; p < size; ++p) {
                if (recorded[static_cast<std::size_t>(p)] != Traits::element_at(solution, l, p)) {
                    return false;
                }
            }
            return true;
        }
    }

    // sequence shape: visit(l) for every list that differs from the snapshot.
    template <typename Visit>
    static void for_each_changed_list(const Solution& solution, const SolutionSnapshot& snap,
                                   Visit&& visit) {
        if constexpr (Traits::shape == Shape::Sequence) {
            const int list_count = Traits::list_count(solution);
            for (int l = 0; l < list_count; ++l) {
                if (!same_list(solution, snap, l)) visit(l);
            }
        } else {
            (void)solution; (void)snap; (void)visit;
        }
    }

    // sequence shape: appends every element of every list that differs from the
    // snapshot (the elements a compound move touched, for the entity tabu).
    static void append_changed_elements(const Solution& solution, const SolutionSnapshot& snap,
                                      std::vector<int>& out) {
        for_each_changed_list(solution, snap, [&](int l) {
            if constexpr (Traits::shape == Shape::Sequence) {
                const int size = Traits::list_size(solution, l);
                for (int p = 0; p < size; ++p) out.push_back(Traits::element_at(solution, l, p));
            }
        });
    }

    // sequence shape: rewrite list `l` to what the snapshot recorded, through the
    // editor's list hooks (one begin_list_edit/end_list_edit pair), so the
    // objective re-costs that list only. The caller reindexes its
    // ElementIndex for `l`.
    static void restore_list(Editor<Solution>& editor, const SolutionSnapshot& snap,
                            int l) {
        if constexpr (Traits::shape == Shape::Sequence) {
            // A list the snapshot never recorded cannot be restored: same_list()
            // reports it as changed, and a caller restoring it has a snapshot
            // of another solution. Loud in every build.
            if (l < 0 || l >= static_cast<int>(snap.lists.size())) {
                throw std::logic_error("SolutionSnapshot::restore_list: list not recorded");
            }
            Solution& solution = editor.solution();
            const std::vector<int>& wanted = snap.lists[static_cast<std::size_t>(l)];
            editor.begin_list_edit(l);
            while (Traits::list_size(solution, l) > 0) {
                Traits::remove_element(solution, l, Traits::list_size(solution, l) - 1);
            }
            for (std::size_t p = 0; p < wanted.size(); ++p) {
                Traits::insert_element(solution, l, static_cast<int>(p), wanted[p]);
            }
            editor.end_list_edit(l);
        } else {
            (void)editor; (void)snap; (void)l;
        }
    }

    static void restore(Editor<Solution>& editor, const SolutionSnapshot& snap) {
        Solution& solution = editor.solution();
        if constexpr (Traits::shape == Shape::Assignment) {
            editor.begin_batch();
            for (int e = 0; e < static_cast<int>(snap.variables.size()); ++e) {
                if (Traits::value_of(solution, e) != snap.variables[e]) {
                    editor.assign(e, snap.variables[e]);
                }
            }
            editor.end_batch();
        } else {
            // One-time rebuild: write the lists directly, then resynchronize
            // the derived state and the objective with a reset.
            for (int l = 0; l < static_cast<int>(snap.lists.size()); ++l) {
                while (Traits::list_size(solution, l) > 0) {
                    Traits::remove_element(solution, l, Traits::list_size(solution, l) - 1);
                }
                for (int p = 0; p < static_cast<int>(snap.lists[l].size()); ++p) {
                    Traits::insert_element(solution, l, p, snap.lists[l][p]);
                }
            }
            editor.rebuild();
        }
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_SNAPSHOT_HPP

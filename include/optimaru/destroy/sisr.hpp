// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_DESTROY_SISR_HPP
#define OPTIMARU_DESTROY_SISR_HPP

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/destroy/random.hpp"
#include "optimaru/search/operators.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// SISR ruin — Slack Induction by Substring Removal
// ---------------------------------------------------------------------------
// Implements SISR (Christiaens & Vanden Berghe, 2020). Instead of removing
// scattered customers, it removes *substrings*
// of consecutive elements from several nearby routes at once: pick a seed
// element, walk its nearest neighbours, and from each neighbour's list cut out
// a contiguous run containing it — one run per list, until enough lists have
// been cut.
//
// Why this beats random/related removal on routing problems: taking a
// contiguous run leaves a genuine hole in a route rather than punching
// isolated gaps, so the recreate can re-thread whole segments and the routes
// gain the slack that lets a different assignment win. Removing the same
// number of scattered elements usually just puts them straight back.
//
// sequence shape only — a "string" is a run of consecutive positions, which has no
// meaning for assignment variables. Requires nearby lists (a configured
// local_search.nearby.distance); without either it delegates to the random ruin.
template <typename Solution>
class SisrRuin final : public RuinOperator<Solution> {
    using Traits = SolutionTraits<Solution>;

public:
    // max_string_removals — how many distinct lists may be cut in one kick.
    // max_string_size     — upper bound on one run's length (also capped by the
    //                     instance's average route length, as in the paper).
    SisrRuin(int max_string_removals, int max_string_size)
        : max_string_removals_(std::max(1, max_string_removals)),
          max_string_size_(std::max(1, max_string_size)) {}

    const char* name() const override { return "sisrRuin"; }

    std::vector<int> select_ruin_set(OperatorContext<Solution>& ctx, int ruin_count,
                                   std::mt19937_64& rng) override {
        if constexpr (Traits::shape != Shape::Sequence) {
            return fallback_.select_ruin_set(ctx, ruin_count, rng);
        } else {
            const Solution& solution = ctx.editor.solution();
            const int list_count = Traits::list_count(solution);
            const int element_count = Traits::element_count(solution);
            if (!ctx.nearby.enabled() || list_count == 0 || element_count == 0) {
                return fallback_.select_ruin_set(ctx, ruin_count, rng);
            }

            // The paper's cardinality rule: a string never exceeds the average
            // route length, so the kick stays proportional to the instance.
            int assigned_total = 0;
            for (int l = 0; l < list_count; ++l) assigned_total += Traits::list_size(solution, l);
            const int average_list_size = assigned_total / list_count;
            const int max_string_size = std::max(1, std::min(max_string_size_, average_list_size));
            const int max_string_removals = std::min(list_count, max_string_removals_);

            std::vector<char> list_cut(static_cast<std::size_t>(list_count), 0);
            std::vector<int> ruined;
            ruined.reserve(static_cast<std::size_t>(ruin_count));
            int cut_lists = 0;

            const int seed = static_cast<int>(rng() % static_cast<std::uint64_t>(element_count));
            // The seed itself first, then outward through its neighbours: each
            // contributes the run around it in its own (not yet cut) list.
            if (!cut_string_around(ctx, seed, list_cut, ruined, max_string_size, ruin_count, rng)) {
                ++cut_lists;
            }
            for (int neighbor : ctx.nearby.of(seed)) {
                if (cut_lists >= max_string_removals) break;
                if (static_cast<int>(ruined.size()) >= ruin_count) break;
                if (!cut_string_around(ctx, neighbor, list_cut, ruined, max_string_size, ruin_count,
                                     rng)) {
                    ++cut_lists;
                }
            }

            // Nothing cuttable (e.g. every neighbour unassigned): rather than
            // hand back an empty kick, let the random ruin produce one.
            if (ruined.empty()) return fallback_.select_ruin_set(ctx, ruin_count, rng);
            return ruined;
        }
    }

private:
    // Cuts the run containing `element` out of its list, appending the removed
    // ids to `ruined`. Returns true when nothing was cut (element unassigned,
    // or its list already cut) so the caller does not count a list removal.
    bool cut_string_around(OperatorContext<Solution>& ctx, int element, std::vector<char>& list_cut,
                         std::vector<int>& ruined, int max_string_size, int ruin_count,
                         std::mt19937_64& rng) {
        const Solution& solution = ctx.editor.solution();
        const int list = ctx.element_list[element];
        if (list < 0) return true;  // currently unassigned
        if (list_cut[static_cast<std::size_t>(list)]) return true;

        const int list_size = Traits::list_size(solution, list);
        if (list_size == 0) return true;

        // Run length in [1, min(list_size, max_string_size)], positioned so that
        // `element` falls somewhere inside it (the paper's random offset).
        const int upper = std::min(list_size, max_string_size);
        const int size = 1 + static_cast<int>(rng() % static_cast<std::uint64_t>(upper));
        const int offset = static_cast<int>(rng() % static_cast<std::uint64_t>(size));
        int start = ctx.element_position[element] - offset;

        for (int k = 0; k < size; ++k) {
            if (static_cast<int>(ruined.size()) >= ruin_count) break;
            // Wrap like a cyclic route, matching SisrDestroy's `idx % len`.
            int position = (start + k) % list_size;
            if (position < 0) position += list_size;
            ruined.push_back(Traits::element_at(solution, list, position));
        }
        list_cut[static_cast<std::size_t>(list)] = 1;
        return false;
    }

    int max_string_removals_;
    int max_string_size_;
    RandomRuin<Solution> fallback_;
};

}  // namespace optimaru

#endif  // OPTIMARU_DESTROY_SISR_HPP

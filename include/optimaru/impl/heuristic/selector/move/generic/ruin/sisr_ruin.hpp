#ifndef OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_RUIN_SISR_RUIN_HPP
#define OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_RUIN_SISR_RUIN_HPP

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/ruin/random_ruin.hpp"
#include "optimaru/preview/api/move/operators.hpp"

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
// LIST kind only — a "string" is a run of consecutive positions, which has no
// meaning for BASIC variables. Requires nearby lists (a configured
// nearbyDistanceMeter); without either it delegates to the random ruin.
template <typename Solution>
class SisrRuin final : public RuinOperator<Solution> {
    using Descriptor = SolutionDescriptor<Solution>;

public:
    // maxStringRemovals — how many distinct lists may be cut in one kick.
    // maxStringSize     — upper bound on one run's length (also capped by the
    //                     instance's average route length, as in the paper).
    SisrRuin(int maxStringRemovals, int maxStringSize)
        : maxStringRemovals_(std::max(1, maxStringRemovals)),
          maxStringSize_(std::max(1, maxStringSize)) {}

    const char* name() const override { return "sisrRuin"; }

    std::vector<int> selectRuinSet(OperatorContext<Solution>& ctx, int ruinCount,
                                   std::mt19937_64& rng) override {
        if constexpr (Descriptor::kind != VariableKind::LIST) {
            return fallback_.selectRuinSet(ctx, ruinCount, rng);
        } else {
            const Solution& solution = ctx.director.workingSolution();
            const int listCount = Descriptor::listCount(solution);
            const int elementCount = Descriptor::elementCount(solution);
            if (!ctx.nearby.enabled() || listCount == 0 || elementCount == 0) {
                return fallback_.selectRuinSet(ctx, ruinCount, rng);
            }

            // The paper's cardinality rule: a string never exceeds the average
            // route length, so the kick stays proportional to the instance.
            int assignedTotal = 0;
            for (int l = 0; l < listCount; ++l) assignedTotal += Descriptor::listSize(solution, l);
            const int averageListSize = assignedTotal / listCount;
            const int maxStringSize = std::max(1, std::min(maxStringSize_, averageListSize));
            const int maxStringRemovals = std::min(listCount, maxStringRemovals_);

            std::vector<char> listCut(static_cast<std::size_t>(listCount), 0);
            std::vector<int> ruined;
            ruined.reserve(static_cast<std::size_t>(ruinCount));
            int cutLists = 0;

            const int seed = static_cast<int>(rng() % static_cast<std::uint64_t>(elementCount));
            // The seed itself first, then outward through its neighbours: each
            // contributes the run around it in its own (not yet cut) list.
            if (!cutStringAround(ctx, seed, listCut, ruined, maxStringSize, ruinCount, rng)) {
                ++cutLists;
            }
            for (int neighbor : ctx.nearby.of(seed)) {
                if (cutLists >= maxStringRemovals) break;
                if (static_cast<int>(ruined.size()) >= ruinCount) break;
                if (!cutStringAround(ctx, neighbor, listCut, ruined, maxStringSize, ruinCount,
                                     rng)) {
                    ++cutLists;
                }
            }

            // Nothing cuttable (e.g. every neighbour unassigned): rather than
            // hand back an empty kick, let the random ruin produce one.
            if (ruined.empty()) return fallback_.selectRuinSet(ctx, ruinCount, rng);
            return ruined;
        }
    }

private:
    // Cuts the run containing `element` out of its list, appending the removed
    // ids to `ruined`. Returns true when nothing was cut (element unassigned,
    // or its list already cut) so the caller does not count a list removal.
    bool cutStringAround(OperatorContext<Solution>& ctx, int element, std::vector<char>& listCut,
                         std::vector<int>& ruined, int maxStringSize, int ruinCount,
                         std::mt19937_64& rng) {
        const Solution& solution = ctx.director.workingSolution();
        const int list = ctx.elementList[element];
        if (list < 0) return true;  // currently unassigned
        if (listCut[static_cast<std::size_t>(list)]) return true;

        const int listSize = Descriptor::listSize(solution, list);
        if (listSize == 0) return true;

        // Run length in [1, min(listSize, maxStringSize)], positioned so that
        // `element` falls somewhere inside it (the paper's random offset).
        const int upper = std::min(listSize, maxStringSize);
        const int size = 1 + static_cast<int>(rng() % static_cast<std::uint64_t>(upper));
        const int offset = static_cast<int>(rng() % static_cast<std::uint64_t>(size));
        int start = ctx.elementPosition[element] - offset;

        for (int k = 0; k < size; ++k) {
            if (static_cast<int>(ruined.size()) >= ruinCount) break;
            // Wrap like a cyclic route, matching SisrDestroy's `idx % len`.
            int position = (start + k) % listSize;
            if (position < 0) position += listSize;
            ruined.push_back(Descriptor::getElement(solution, list, position));
        }
        listCut[static_cast<std::size_t>(list)] = 1;
        return false;
    }

    int maxStringRemovals_;
    int maxStringSize_;
    RandomRuin<Solution> fallback_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_RUIN_SISR_RUIN_HPP

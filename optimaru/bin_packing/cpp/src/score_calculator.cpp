#include "bp/score_calculator.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "bp/constraints/bin_capacity_cost.hpp"
#include "bp/constraints/color_conflict_cost.hpp"
#include "bp/constraints/item_conflict_cost.hpp"
#include "bp/constraints/used_bins_cost.hpp"

namespace bp {

namespace {

// The monolithic score is the default assembly, so it reads the same
// constants the components carry. Integral, so both levels are computed in
// int64 exactly.
constexpr auto kBinCapacityWeight = static_cast<std::int64_t>(BinCapacityCost::kDefaultWeight);
constexpr auto kColorConflictWeight = static_cast<std::int64_t>(ColorConflictCost::kDefaultWeight);
constexpr auto kItemConflictWeight = static_cast<std::int64_t>(ItemConflictCost::kDefaultWeight);
constexpr auto kUsedBinsWeight = static_cast<std::int64_t>(UsedBinsCost::kDefaultWeight);
static_assert(kBinCapacityWeight == BinCapacityCost::kDefaultWeight &&
                      kColorConflictWeight == ColorConflictCost::kDefaultWeight &&
                      kItemConflictWeight == ItemConflictCost::kDefaultWeight &&
                      kUsedBinsWeight == UsedBinsCost::kDefaultWeight,
              "the monolithic calculator assumes integral default weights");

}  // namespace

void ScoreCalculator::requireBinsInRange(const BinPackingProblem& problem) {
    const int binCount = problem.binCount();
    for (const Item& item : problem.items) {
        if (item.bin < -1 || item.bin >= binCount) {
            throw std::invalid_argument("item '" + item.id + "' has bin " +
                                        std::to_string(item.bin) + ", outside -1.." +
                                        std::to_string(binCount - 1));
        }
    }
}

void ScoreCalculator::prepare(BinPackingProblem& problem) {
    // binCapacity stays writable after construction (Python: bin_capacity)
    // and `prepared` does not cover it, so it is checked on every call.
    if (problem.binCapacity <= 0) throw std::invalid_argument("bin_capacity must be positive");
    requireBinsInRange(problem);
    if (problem.prepared) return;

    const int n = problem.itemCount();
    std::unordered_map<std::string, int> byId;
    byId.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const Item& item = problem.items[i];
        if (item.weight <= 0) throw std::invalid_argument("item weights must be positive");
        if (!byId.emplace(item.id, i).second)
            throw std::invalid_argument("item ids must be unique");
    }

    // Colour classes: only colours two or more items carry can conflict.
    std::unordered_map<std::string, int> colorCount;
    for (const Item& item : problem.items) {
        if (item.color) ++colorCount[*item.color];
    }
    std::unordered_map<std::string, int> classOf;
    problem.colorClass.assign(static_cast<std::size_t>(n), -1);
    for (int i = 0; i < n; ++i) {
        const Item& item = problem.items[i];
        if (!item.color || colorCount[*item.color] < 2) continue;
        const auto inserted = classOf.emplace(*item.color, static_cast<int>(classOf.size()));
        problem.colorClass[i] = inserted.first->second;
    }
    problem.colorClassCount = static_cast<int>(classOf.size());

    problem.conflictPairs.clear();
    problem.conflictPairs.reserve(problem.conflicts.size());
    problem.conflictPartners.assign(static_cast<std::size_t>(n), {});
    for (const auto& [first, second] : problem.conflicts) {
        const auto left = byId.find(first);
        const auto right = byId.find(second);
        if (left == byId.end() || right == byId.end() || first == second) {
            throw std::invalid_argument("conflict endpoints must be two different item ids (got '" +
                                        first + "', '" + second + "')");
        }
        problem.conflictPairs.emplace_back(left->second, right->second);
        problem.conflictPartners[left->second].push_back(right->second);
        problem.conflictPartners[right->second].push_back(left->second);
    }

    problem.prepared = true;
}

RawCosts ScoreCalculator::rawCosts(BinPackingProblem& problem) {
    prepare(problem);

    const int n = problem.itemCount();
    std::vector<std::int64_t> load(static_cast<std::size_t>(n), 0);
    std::vector<int> count(static_cast<std::size_t>(n), 0);
    for (const Item& item : problem.items) {
        if (!item.isAssigned()) continue;  // unassigned: contributes nothing
        load[item.bin] += item.weight;
        ++count[item.bin];
    }

    // hard 1: binCapacityCost; soft 4: usedBinsCost.
    std::int64_t overCapacity = 0;
    std::int64_t usedBins = 0;
    for (int b = 0; b < n; ++b) {
        if (count[b] == 0) continue;
        ++usedBins;
        if (load[b] > problem.binCapacity) overCapacity += load[b] - problem.binCapacity;
    }

    // hard 2: colorConflictCost — every item beyond the first of its colour
    // in a bin.
    std::int64_t colorExcess = 0;
    std::unordered_map<std::int64_t, int> seen;
    for (int i = 0; i < n; ++i) {
        const Item& item = problem.items[i];
        const int cls = problem.colorClass[i];
        if (!item.isAssigned() || cls < 0) continue;
        const std::int64_t key =
                static_cast<std::int64_t>(item.bin) * problem.colorClassCount + cls;
        if (seen[key]++ > 0) ++colorExcess;
    }

    // hard 3: itemConflictCost — listed pairs sharing a bin.
    std::int64_t conflicts = 0;
    for (const auto& [a, b] : problem.conflictPairs) {
        const int binA = problem.items[a].bin;
        if (binA >= 0 && binA == problem.items[b].bin) ++conflicts;
    }

    RawCosts raw;
    raw.binCapacity = overCapacity;
    raw.colorConflict = colorExcess;
    raw.itemConflict = conflicts;
    raw.usedBins = usedBins;
    return raw;
}

Score ScoreCalculator::score(BinPackingProblem& problem) {
    const RawCosts raw = rawCosts(problem);
    Score score;
    score.hard = -(kBinCapacityWeight * raw.binCapacity + kColorConflictWeight * raw.colorConflict +
                   kItemConflictWeight * raw.itemConflict);
    // Negate in integer space so a zero penalty prints as 0, not -0.
    score.soft = static_cast<double>(-(kUsedBinsWeight * raw.usedBins));
    problem.score = score;
    return score;
}

}  // namespace bp

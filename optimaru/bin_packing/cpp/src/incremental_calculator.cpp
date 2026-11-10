#include "bp/incremental_calculator.hpp"

#include <cstddef>
#include <cstdint>

#include "bp/constraints/bin_capacity_cost.hpp"
#include "bp/constraints/color_conflict_cost.hpp"
#include "bp/constraints/item_conflict_cost.hpp"
#include "bp/constraints/used_bins_cost.hpp"
#include "bp/score_calculator.hpp"

namespace bp {

namespace {

// The same constants the components and ScoreCalculator read (integral; see
// score_calculator.cpp).
constexpr auto kBinCapacityWeight = static_cast<std::int64_t>(BinCapacityCost::kDefaultWeight);
constexpr auto kColorConflictWeight = static_cast<std::int64_t>(ColorConflictCost::kDefaultWeight);
constexpr auto kItemConflictWeight = static_cast<std::int64_t>(ItemConflictCost::kDefaultWeight);
constexpr auto kUsedBinsWeight = static_cast<std::int64_t>(UsedBinsCost::kDefaultWeight);

}  // namespace

void BinPackingIncrementalScoreCalculator::rebuild(BinPackingProblem& solution) {
    ScoreCalculator::prepare(solution);  // derived lookups
    problem_ = &solution;

    const std::size_t n = solution.items.size();
    load_.assign(n, 0);
    count_.assign(n, 0);
    const std::size_t cells = n * static_cast<std::size_t>(solution.colorClassCount);
    flatColors_ = cells <= maxFlatColorCells_;
    colorCountFlat_.assign(flatColors_ ? cells : 0, 0);
    colorCountMap_.clear();
    overCapacity_ = 0;
    colorExcess_ = 0;
    conflicts_ = 0;
    usedBins_ = 0;

    assign_.assign(n, -1);
    for (int i = 0; i < solution.itemCount(); ++i) {
        const int bin = solution.items[i].bin;
        if (bin >= 0) insert(i, bin);
    }
}

int& BinPackingIncrementalScoreCalculator::colorCount(int bin, int cls) {
    const std::size_t key =
            static_cast<std::size_t>(bin) * static_cast<std::size_t>(problem_->colorClassCount) +
            static_cast<std::size_t>(cls);
    if (flatColors_) return colorCountFlat_[key];
    return colorCountMap_[static_cast<std::int64_t>(key)];
}

// Adds item `itemIndex` to bin `bin`, updating each aggregate and total.
void BinPackingIncrementalScoreCalculator::insert(int itemIndex, int bin) {
    const Item& item = problem_->items[itemIndex];

    // hard 1: binCapacityCost — replace this bin's penalty term.
    overCapacity_ -= capacityPenalty(bin);
    load_[bin] += item.weight;
    overCapacity_ += capacityPenalty(bin);

    // soft 4: usedBinsCost — the first item switches the bin on.
    if (++count_[bin] == 1) ++usedBins_;

    // hard 2: colorConflictCost.
    const int cls = problem_->colorClass[itemIndex];
    if (cls >= 0 && colorCount(bin, cls)++ > 0) ++colorExcess_;

    // hard 3: itemConflictCost — partners already in the bin.
    for (int partner : problem_->conflictPartners[itemIndex]) {
        if (assign_[partner] == bin) ++conflicts_;
    }
    assign_[itemIndex] = bin;
}

// Exact mirror of insert.
void BinPackingIncrementalScoreCalculator::retract(int itemIndex, int bin) {
    const Item& item = problem_->items[itemIndex];
    assign_[itemIndex] = -1;

    overCapacity_ -= capacityPenalty(bin);
    load_[bin] -= item.weight;
    overCapacity_ += capacityPenalty(bin);

    if (--count_[bin] == 0) --usedBins_;

    const int cls = problem_->colorClass[itemIndex];
    if (cls >= 0 && --colorCount(bin, cls) > 0) --colorExcess_;

    for (int partner : problem_->conflictPartners[itemIndex]) {
        if (assign_[partner] == bin) --conflicts_;
    }
}

void BinPackingIncrementalScoreCalculator::before_assign(int itemIndex) {
    const int old = assign_[itemIndex];
    if (old >= 0) retract(itemIndex, old);  // unassigned: nothing to retract
}

void BinPackingIncrementalScoreCalculator::after_assign(int itemIndex) {
    const int now = problem_->items[itemIndex].bin;  // the new value
    if (now >= 0) insert(itemIndex, now);            // now unassigned: nothing to insert
}

Score BinPackingIncrementalScoreCalculator::score() {
    Score score;
    score.hard = -(kBinCapacityWeight * overCapacity_ + kColorConflictWeight * colorExcess_ +
                   kItemConflictWeight * conflicts_);
    // Negate in integer space so a zero penalty prints as 0, not -0.
    score.soft = static_cast<double>(-(kUsedBinsWeight * usedBins_));
    return score;
}

}  // namespace bp

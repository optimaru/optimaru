#ifndef BP_INCREMENTAL_CALCULATOR_HPP
#define BP_INCREMENTAL_CALCULATOR_HPP

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "bp/domain.hpp"
#include "bp/score_calculator.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace bp
{

// The bin-packing implementation of the engine's
// optimaru::IncrementalScoreCalculator.
//
// Every constraint is a function of per-bin aggregates, so a single variable
// change is O(1) plus the moved item's conflict partners:
//   - hard 1 (binCapacityCost): contribution(b) = max(0, load_b - capacity);
//     moving one item in/out swaps only that bin's term.
//   - hard 2 (colorConflictCost): per (bin, colour class) counts; an item
//     joining a bin that already holds its colour adds 1, leaving one that
//     still holds it removes 1.
//   - hard 3 (itemConflictCost): the moved item's partners (with
//     multiplicity) in the bin it leaves / joins.
//   - soft 4 (usedBinsCost): a bin switches on when its item count goes
//     0 -> 1 and off when it drops back to 0.
//
// beforeVariableChanged retracts the item's contribution while `bin` still
// holds the OLD value; afterVariableChanged inserts the NEW value. The
// calculator keeps its own copy of the assignment (`assign_`), so partner
// lookups never depend on the order in which a compound move notifies.
// Unassigned items (bin == -1) contribute nothing. The full-recompute
// reference is ScoreCalculator::calculateScore (checked in the tests and
// under EnvironmentMode::FULL_ASSERT).
class BinPackingIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<BinPackingProblem>
{
public:
    // Above this many (bin, colour class) cells the colour counts live in a
    // hash map instead of a flat array.
    static constexpr std::size_t kDefaultMaxFlatColorCells
        = std::size_t{1} << 22;

    BinPackingIncrementalScoreCalculator() = default;
    // A different flat-array limit (0 = always the hash map, so the tests can
    // cover that path on small instances).
    explicit BinPackingIncrementalScoreCalculator(std::size_t maxFlatColorCells)
        : maxFlatColorCells_(maxFlatColorCells)
    {
    }

    void resetWorkingSolution(BinPackingProblem &workingSolution) override;
    void beforeVariableChanged(int itemIndex) override;
    void afterVariableChanged(int itemIndex) override;
    Score calculateScore() override;

    // The running raw totals calculateScore weighs; equal to
    // ScoreCalculator::rawCosts on the current assignment.
    RawCosts totals() const
    {
        RawCosts raw;
        raw.binCapacity = overCapacity_;
        raw.colorConflict = colorExcess_;
        raw.itemConflict = conflicts_;
        raw.usedBins = usedBins_;
        return raw;
    }
    // Whether the last reset chose the flat colour-count array.
    bool flatColorCounts() const { return flatColors_; }

private:
    std::size_t maxFlatColorCells_ = kDefaultMaxFlatColorCells;
    BinPackingProblem *problem_ = nullptr;
    std::vector<int> assign_;  // bin per item (-1 == unassigned)

    // Per-bin aggregates.
    std::vector<std::int64_t> load_;
    std::vector<int> count_;
    // Items per (bin, colour class): a flat bin * classCount + class array,
    // or a hash map when that array would be large.
    bool flatColors_ = true;
    std::vector<int> colorCountFlat_;
    std::unordered_map<std::int64_t, int> colorCountMap_;

    // Running totals (all counted positive; signs/weights applied in
    // calculateScore).
    std::int64_t overCapacity_ = 0;  // hard 1
    std::int64_t colorExcess_ = 0;   // hard 2
    std::int64_t conflicts_ = 0;     // hard 3
    std::int64_t usedBins_ = 0;      // soft 4

    void insert(int itemIndex, int bin);
    void retract(int itemIndex, int bin);
    int &colorCount(int bin, int cls);

    std::int64_t capacityPenalty(int bin) const
    {
        const std::int64_t over = load_[bin] - problem_->binCapacity;
        return over > 0 ? over : 0;
    }
};

}  // namespace bp

#endif  // BP_INCREMENTAL_CALCULATOR_HPP

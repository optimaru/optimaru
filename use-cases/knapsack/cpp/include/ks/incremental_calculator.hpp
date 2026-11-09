#ifndef KS_INCREMENTAL_CALCULATOR_HPP
#define KS_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "ks/domain.hpp"
#include "ks/score_calculator.hpp"
#include "optimaru/objective.hpp"

namespace ks
{

// The knapsack implementation of the engine's
// optimaru::Objective: the monolithic fast path of the
// default constraints (ks/score_calculator.hpp lists them).
//
// It keeps a copy of the selection and the aggregates of every constraint —
// the selected weight and value, the usage per declared resource and the
// overflow sum, the unselected mandatory and selected forbidden entries, the
// selected count per group and the violation sum, the unmet dependencies — so
// flipping one item costs O(its resources + groups + dependencies).
//
// before_assign is a no-op (the copy still holds the old state);
// after_assign compares the item's new value with the copy and, if
// "selected" changed, retracts and re-inserts the touched terms. -1
// (undecided) counts as not selected. The full-recompute reference is
// ScoreCalculator::score (checked in the tests and under
// ScoreChecks::ScoreChecks::Full). Requires ScoreCalculator::prepare(problem).
class KnapsackIncrementalScoreCalculator final
    : public optimaru::Objective<KnapsackProblem>
{
public:
    void rebuild(KnapsackProblem &solution) override;
    void before_assign(int itemIndex) override;
    void after_assign(int itemIndex) override;
    Score score() override;

    // The raw cost of every default constraint, read from the running
    // totals (the same fields as ScoreCalculator::rawCosts).
    // score() weighs exactly these; the tests compare them with
    // ScoreCalculator::rawCosts after every move.
    RawCosts totals() const;

private:
    KnapsackProblem *problem_ = nullptr;
    std::vector<char> selected_;  // the selection as last seen

    std::vector<std::int64_t> usage_;  // per declared resource
    std::vector<std::int64_t> groupCount_;  // selected items per group

    // Running raw totals, all >= 0 (signs and weights applied in
    // score).
    std::int64_t load_ = 0;              // knapsackCapacityCost input
    std::int64_t resourceOverflow_ = 0;  // resourceCapacityCost
    std::int64_t missingMandatory_ = 0;  // mandatoryItemsCost
    std::int64_t chosenForbidden_ = 0;   // forbiddenItemsCost
    std::int64_t groupViolation_ = 0;    // selectionGroupBoundsCost
    std::int64_t unmetDependencies_ = 0; // itemDependencyCost
    std::int64_t value_ = 0;             // selectedValueCost (as +value)

    void flip(int itemIndex, bool selected);
    std::int64_t resourceOverflowOf(int resource) const;
    std::int64_t groupViolationOf(int group) const;
    std::int64_t unmetOf(int dependency) const;
};

}  // namespace ks

#endif  // KS_INCREMENTAL_CALCULATOR_HPP

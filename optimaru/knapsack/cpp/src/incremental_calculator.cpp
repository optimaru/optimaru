#include "ks/incremental_calculator.hpp"

#include <cstdint>
#include <stdexcept>

#include "ks/constraints/forbidden_items_cost.hpp"
#include "ks/constraints/item_dependency_cost.hpp"
#include "ks/constraints/knapsack_capacity_cost.hpp"
#include "ks/constraints/mandatory_items_cost.hpp"
#include "ks/constraints/resource_capacity_cost.hpp"
#include "ks/constraints/selected_value_cost.hpp"
#include "ks/constraints/selection_group_bounds_cost.hpp"

namespace ks {

namespace {

constexpr auto kCapacityWeight = static_cast<std::int64_t>(KnapsackCapacityCost::kDefaultWeight);
constexpr auto kResourceWeight = static_cast<std::int64_t>(ResourceCapacityCost::kDefaultWeight);
constexpr auto kMandatoryWeight = static_cast<std::int64_t>(MandatoryItemsCost::kDefaultWeight);
constexpr auto kForbiddenWeight = static_cast<std::int64_t>(ForbiddenItemsCost::kDefaultWeight);
constexpr auto kGroupWeight = static_cast<std::int64_t>(SelectionGroupBoundsCost::kDefaultWeight);
constexpr auto kDependencyWeight = static_cast<std::int64_t>(ItemDependencyCost::kDefaultWeight);
constexpr auto kValueWeight = static_cast<std::int64_t>(SelectedValueCost::kDefaultWeight);
static_assert(kCapacityWeight == KnapsackCapacityCost::kDefaultWeight &&
                      kResourceWeight == ResourceCapacityCost::kDefaultWeight &&
                      kMandatoryWeight == MandatoryItemsCost::kDefaultWeight &&
                      kForbiddenWeight == ForbiddenItemsCost::kDefaultWeight &&
                      kGroupWeight == SelectionGroupBoundsCost::kDefaultWeight &&
                      kDependencyWeight == ItemDependencyCost::kDefaultWeight &&
                      kValueWeight == SelectedValueCost::kDefaultWeight,
              "the monolithic calculator assumes integral default weights");

}  // namespace

void KnapsackIncrementalScoreCalculator::rebuild(KnapsackProblem& solution) {
    if (!solution.prepared) {
        throw std::logic_error(
                "KnapsackIncrementalScoreCalculator requires "
                "ScoreCalculator::prepare(problem) first");
    }
    problem_ = &solution;
    const KnapsackProblem& problem = solution;
    const int n = problem.itemCount();
    selected_.assign(n, 0);
    usage_.assign(problem.resources.size(), 0);
    groupCount_.assign(problem.selectionGroups.size(), 0);
    load_ = 0;
    value_ = 0;
    missingMandatory_ = 0;
    chosenForbidden_ = 0;
    for (int i = 0; i < n; ++i) {
        const bool on = problem.items[i].isSelected();
        selected_[i] = on ? 1 : 0;
        if (on) {
            load_ += problem.items[i].weight;
            value_ += problem.items[i].value;
            for (const auto& [resource, amount] : problem.itemResources[i])
                usage_[resource] += amount;
            for (int group : problem.itemGroups[i]) ++groupCount_[group];
            chosenForbidden_ += problem.forbiddenCount[i];
        } else {
            missingMandatory_ += problem.mandatoryCount[i];
        }
    }
    resourceOverflow_ = 0;
    for (std::size_t r = 0; r < usage_.size(); ++r)
        resourceOverflow_ += resourceOverflowOf(static_cast<int>(r));
    groupViolation_ = 0;
    for (std::size_t g = 0; g < groupCount_.size(); ++g)
        groupViolation_ += groupViolationOf(static_cast<int>(g));
    unmetDependencies_ = 0;
    for (std::size_t d = 0; d < problem.dependencies.size(); ++d)
        unmetDependencies_ += unmetOf(static_cast<int>(d));
}

void KnapsackIncrementalScoreCalculator::before_assign(int /*itemIndex*/) {
    // selected_ still holds the old state; after_assign diffs it.
}

void KnapsackIncrementalScoreCalculator::after_assign(int itemIndex) {
    const bool on = problem_->items[itemIndex].isSelected();
    if (on != static_cast<bool>(selected_[itemIndex])) flip(itemIndex, on);
}

void KnapsackIncrementalScoreCalculator::flip(int itemIndex, bool on) {
    const KnapsackProblem& problem = *problem_;
    const Item& item = problem.items[itemIndex];
    const std::int64_t sign = on ? 1 : -1;
    load_ += sign * item.weight;
    value_ += sign * item.value;
    for (const auto& [resource, amount] : problem.itemResources[itemIndex]) {
        resourceOverflow_ -= resourceOverflowOf(resource);
        usage_[resource] += sign * amount;
        resourceOverflow_ += resourceOverflowOf(resource);
    }
    missingMandatory_ -= sign * problem.mandatoryCount[itemIndex];
    chosenForbidden_ += sign * problem.forbiddenCount[itemIndex];
    for (int group : problem.itemGroups[itemIndex]) {
        groupViolation_ -= groupViolationOf(group);
        groupCount_[group] += sign;
        groupViolation_ += groupViolationOf(group);
    }
    for (int d : problem.dependenciesAsItem[itemIndex]) unmetDependencies_ -= unmetOf(d);
    for (int d : problem.dependenciesAsRequired[itemIndex]) unmetDependencies_ -= unmetOf(d);
    selected_[itemIndex] = on ? 1 : 0;
    for (int d : problem.dependenciesAsItem[itemIndex]) unmetDependencies_ += unmetOf(d);
    for (int d : problem.dependenciesAsRequired[itemIndex]) unmetDependencies_ += unmetOf(d);
}

std::int64_t KnapsackIncrementalScoreCalculator::resourceOverflowOf(int resource) const {
    const std::int64_t over = usage_[resource] - problem_->resources[resource].capacity;
    return over > 0 ? over : 0;
}

std::int64_t KnapsackIncrementalScoreCalculator::groupViolationOf(int group) const {
    const SelectionGroup& g = problem_->selectionGroups[group];
    const std::int64_t n = groupCount_[group];
    const std::int64_t under = g.minSelected - n;
    const std::int64_t over = n - g.effectiveMax();
    return (under > 0 ? under : 0) + (over > 0 ? over : 0);
}

std::int64_t KnapsackIncrementalScoreCalculator::unmetOf(int dependency) const {
    const Dependency& d = problem_->dependencies[dependency];
    return selected_[d.item] && !selected_[d.required] ? 1 : 0;
}

RawCosts KnapsackIncrementalScoreCalculator::totals() const {
    RawCosts raw;
    raw.capacityOverflow = load_ > problem_->capacity ? load_ - problem_->capacity : 0;
    raw.resourceOverflow = resourceOverflow_;
    raw.missingMandatory = missingMandatory_;
    raw.chosenForbidden = chosenForbidden_;
    raw.groupViolation = groupViolation_;
    raw.unmetDependencies = unmetDependencies_;
    raw.value = value_;
    return raw;
}

Score KnapsackIncrementalScoreCalculator::score() {
    const RawCosts raw = totals();
    Score score;
    score.hard =
            -(kCapacityWeight * raw.capacityOverflow + kResourceWeight * raw.resourceOverflow +
              kMandatoryWeight * raw.missingMandatory + kForbiddenWeight * raw.chosenForbidden +
              kGroupWeight * raw.groupViolation + kDependencyWeight * raw.unmetDependencies);
    score.soft = static_cast<double>(kValueWeight * raw.value);
    return score;
}

}  // namespace ks

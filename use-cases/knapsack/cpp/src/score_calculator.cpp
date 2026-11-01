#include "ks/score_calculator.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ks/constraints/forbidden_items_cost.hpp"
#include "ks/constraints/item_dependency_cost.hpp"
#include "ks/constraints/knapsack_capacity_cost.hpp"
#include "ks/constraints/mandatory_items_cost.hpp"
#include "ks/constraints/resource_capacity_cost.hpp"
#include "ks/constraints/selected_value_cost.hpp"
#include "ks/constraints/selection_group_bounds_cost.hpp"

namespace ks
{

namespace
{

constexpr auto kCapacityWeight = static_cast<std::int64_t>(KnapsackCapacityCost::kDefaultWeight);
constexpr auto kResourceWeight = static_cast<std::int64_t>(ResourceCapacityCost::kDefaultWeight);
constexpr auto kMandatoryWeight = static_cast<std::int64_t>(MandatoryItemsCost::kDefaultWeight);
constexpr auto kForbiddenWeight = static_cast<std::int64_t>(ForbiddenItemsCost::kDefaultWeight);
constexpr auto kGroupWeight = static_cast<std::int64_t>(SelectionGroupBoundsCost::kDefaultWeight);
constexpr auto kDependencyWeight = static_cast<std::int64_t>(ItemDependencyCost::kDefaultWeight);
constexpr auto kValueWeight = static_cast<std::int64_t>(SelectedValueCost::kDefaultWeight);
static_assert(kCapacityWeight == KnapsackCapacityCost::kDefaultWeight
                  && kResourceWeight == ResourceCapacityCost::kDefaultWeight
                  && kMandatoryWeight == MandatoryItemsCost::kDefaultWeight
                  && kForbiddenWeight == ForbiddenItemsCost::kDefaultWeight
                  && kGroupWeight == SelectionGroupBoundsCost::kDefaultWeight
                  && kDependencyWeight == ItemDependencyCost::kDefaultWeight
                  && kValueWeight == SelectedValueCost::kDefaultWeight,
              "the monolithic calculator assumes integral default weights");
static_assert(KnapsackCapacityCost::kDefaultLevel == optimaru::Level::Hard
                  && ResourceCapacityCost::kDefaultLevel == optimaru::Level::Hard
                  && MandatoryItemsCost::kDefaultLevel == optimaru::Level::Hard
                  && ForbiddenItemsCost::kDefaultLevel == optimaru::Level::Hard
                  && SelectionGroupBoundsCost::kDefaultLevel == optimaru::Level::Hard
                  && ItemDependencyCost::kDefaultLevel == optimaru::Level::Hard
                  && SelectedValueCost::kDefaultLevel == optimaru::Level::Soft,
              "the monolithic calculator assumes six hard constraints and a soft reward");

[[noreturn]] void invalid(const std::string &message)
{
    throw std::invalid_argument(message);
}

void requireItem(const KnapsackProblem &problem, int index, const std::string &where)
{
    if (index < 0 || index >= problem.itemCount())
    {
        invalid(where + ": item index " + std::to_string(index) + " is out of range (0.."
                + std::to_string(problem.itemCount() - 1) + ")");
    }
}

}  // namespace

void ScoreCalculator::prepare(KnapsackProblem &problem)
{
    const int n = problem.itemCount();
    if (problem.capacity < 0)
        invalid("capacity must be >= 0, not " + std::to_string(problem.capacity));

    std::unordered_map<std::string, int> resourceIndex;
    for (std::size_t r = 0; r < problem.resources.size(); ++r)
    {
        const Resource &resource = problem.resources[r];
        if (resource.capacity < 0)
            invalid("resource '" + resource.id + "': capacity must be >= 0");
        if (!resourceIndex.emplace(resource.id, static_cast<int>(r)).second)
            invalid("resource '" + resource.id + "' is declared more than once");
    }

    std::unordered_set<std::string> itemIds;
    problem.itemResources.assign(n, {});
    for (int i = 0; i < n; ++i)
    {
        const Item &item = problem.items[i];
        const std::string where = "item '" + item.id + "'";
        if (!itemIds.insert(item.id).second)
            invalid("item id '" + item.id + "' is used more than once");
        if (item.weight <= 0)
            invalid(where + ": weight must be > 0, not " + std::to_string(item.weight));
        if (item.value < 0)
            invalid(where + ": value must be >= 0, not " + std::to_string(item.value));
        if (item.selected < kUndecided || item.selected > kSelected)
        {
            invalid(where + ": selected must be -1, 0 or 1, not " + std::to_string(item.selected));
        }
        for (const auto &[resource, amount] : item.resourceUsage)
        {
            if (amount < 0)
                invalid(where + ": usage of resource '" + resource + "' must be >= 0");
            const auto found = resourceIndex.find(resource);
            if (found != resourceIndex.end() && amount != 0)
                problem.itemResources[i].emplace_back(found->second, amount);
        }
        for (int required : item.requiredItems)
            requireItem(problem, required, where + " requires");
    }

    problem.mandatoryCount.assign(n, 0);
    for (int index : problem.mandatoryItems)
    {
        requireItem(problem, index, "mandatory items");
        ++problem.mandatoryCount[index];
    }
    problem.forbiddenCount.assign(n, 0);
    for (int index : problem.forbiddenItems)
    {
        requireItem(problem, index, "forbidden items");
        ++problem.forbiddenCount[index];
    }
    for (int i = 0; i < n; ++i)
    {
        if (problem.mandatoryCount[i] > 0 && problem.forbiddenCount[i] > 0)
            invalid("item '" + problem.items[i].id + "' cannot be both mandatory and forbidden");
    }

    problem.itemGroups.assign(n, {});
    for (std::size_t g = 0; g < problem.selectionGroups.size(); ++g)
    {
        const SelectionGroup &group = problem.selectionGroups[g];
        const std::string where = "selection group '" + group.id + "'";
        std::unordered_set<int> seen;
        for (int index : group.items)
        {
            requireItem(problem, index, where);
            if (!seen.insert(index).second)
                invalid(where + ": item ids must be unique");
            problem.itemGroups[index].push_back(static_cast<int>(g));
        }
        const int size = static_cast<int>(group.items.size());
        if (group.minSelected < 0 || (group.maxSelected && *group.maxSelected < 0))
            invalid(where + ": bounds must be >= 0");
        if (group.minSelected > group.effectiveMax() || group.effectiveMax() > size)
            invalid(where + ": bounds must fit the number of items");
    }

    problem.dependencies.clear();
    problem.dependenciesAsItem.assign(n, {});
    problem.dependenciesAsRequired.assign(n, {});
    for (int i = 0; i < n; ++i)
    {
        for (int required : problem.items[i].requiredItems)
        {
            const int d = static_cast<int>(problem.dependencies.size());
            problem.dependencies.push_back({i, required});
            if (required == i)
                continue;  // a self-dependency never counts
            problem.dependenciesAsItem[i].push_back(d);
            problem.dependenciesAsRequired[required].push_back(d);
        }
    }
    problem.prepared = true;
}

RawCosts ScoreCalculator::rawCosts(KnapsackProblem &problem)
{
    prepare(problem);
    const int n = problem.itemCount();
    auto selected = [&problem](int i) { return problem.items[i].isSelected(); };

    // hard 1. knapsackCapacityCost
    std::int64_t load = 0;
    std::int64_t value = 0;
    for (int i = 0; i < n; ++i)
    {
        if (selected(i))
        {
            load += problem.items[i].weight;
            value += problem.items[i].value;
        }
    }
    const std::int64_t capacityOverflow = load > problem.capacity ? load - problem.capacity : 0;

    // hard 2. resourceCapacityCost (declared resources only)
    std::int64_t resourceOverflow = 0;
    for (const Resource &resource : problem.resources)
    {
        std::int64_t usage = 0;
        for (int i = 0; i < n; ++i)
        {
            if (!selected(i))
                continue;
            const auto found = problem.items[i].resourceUsage.find(resource.id);
            if (found != problem.items[i].resourceUsage.end())
                usage += found->second;
        }
        if (usage > resource.capacity)
            resourceOverflow += usage - resource.capacity;
    }

    // hard 3. / 4. mandatoryItemsCost / forbiddenItemsCost (per entry)
    std::int64_t missingMandatory = 0;
    for (int index : problem.mandatoryItems)
        missingMandatory += selected(index) ? 0 : 1;
    std::int64_t chosenForbidden = 0;
    for (int index : problem.forbiddenItems)
        chosenForbidden += selected(index) ? 1 : 0;

    // hard 5. selectionGroupBoundsCost
    std::int64_t groupViolation = 0;
    for (const SelectionGroup &group : problem.selectionGroups)
    {
        std::int64_t count = 0;
        for (int index : group.items)
            count += selected(index) ? 1 : 0;
        const std::int64_t under = group.minSelected - count;
        const std::int64_t over = count - group.effectiveMax();
        groupViolation += (under > 0 ? under : 0) + (over > 0 ? over : 0);
    }

    // hard 6. itemDependencyCost
    std::int64_t unmetDependencies = 0;
    for (int i = 0; i < n; ++i)
    {
        if (!selected(i))
            continue;
        for (int required : problem.items[i].requiredItems)
            unmetDependencies += selected(required) ? 0 : 1;
    }

    RawCosts raw;
    raw.capacityOverflow = capacityOverflow;
    raw.resourceOverflow = resourceOverflow;
    raw.missingMandatory = missingMandatory;
    raw.chosenForbidden = chosenForbidden;
    raw.groupViolation = groupViolation;
    raw.unmetDependencies = unmetDependencies;
    raw.value = value;
    return raw;
}

Score ScoreCalculator::calculateScore(KnapsackProblem &problem)
{
    const RawCosts raw = rawCosts(problem);
    Score score;
    score.hard = -(kCapacityWeight * raw.capacityOverflow + kResourceWeight * raw.resourceOverflow
                   + kMandatoryWeight * raw.missingMandatory + kForbiddenWeight * raw.chosenForbidden
                   + kGroupWeight * raw.groupViolation + kDependencyWeight * raw.unmetDependencies);
    // soft 7. selectedValueCost: raw -(value), so the soft score is +value.
    score.soft = static_cast<double>(kValueWeight * raw.value);
    problem.score = score;
    return score;
}

}  // namespace ks

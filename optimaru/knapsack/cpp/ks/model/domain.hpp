#ifndef KS_DOMAIN_HPP
#define KS_DOMAIN_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"

namespace ks {

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// The engine's Score, so the mapping is the identity and there is no
// medium level (no optimaru::MediumFoldOf specialization). At the default
// weights (the components' kDefaultWeight, ks/constraints/*.hpp):
//   hard = -(weight overflow + resource overflows + unselected mandatory
//            entries + selected forbidden entries + selection-group
//            shortfalls/excesses + unmet item dependencies)
//   soft = +(total selected value)       // selectedValueCost is a reward
// The problem carries no weights. Every level is integral; soft is stored in
// a double, exact below 2^53.
using Score = optimaru::Score;

// The decision variable's values. -1 exists only transiently (the engine's
// ruin-and-recreate, or a C++ caller leaving an item undecided); every
// calculator counts it as "not selected".
constexpr int kUndecided = -1;
constexpr int kNotSelected = 0;
constexpr int kSelected = 1;

// ---------------------------------------------------------------------------
// Resource  (problem fact)
// ---------------------------------------------------------------------------
// One additional capacity dimension (the model's resource_capacities entry).
// Kept as a list in declaration order: that order is the constraint's term
// order, and a dict would come through pybind11 as a sorted std::map.
struct Resource {
    std::string id;
    std::int64_t capacity = 0;
};

// ---------------------------------------------------------------------------
// Item  (entity)
// ---------------------------------------------------------------------------
// `selected` is the single assignment decision variable: 1 = selected, 0 = not
// selected, -1 = undecided (counted as not selected).
struct Item {
    std::string id;
    std::int64_t weight = 1;
    std::int64_t value = 0;
    // Consumption per resource id; ids without a declared Resource are
    // ignored (as in the Python reference).
    std::map<std::string, std::int64_t> resourceUsage;
    // Indices of the items that must be selected with this one (the model's
    // `requires`, a C++20 keyword). Duplicates are separate dependencies; a
    // self-dependency is never violated.
    std::vector<int> requiredItems;

    int selected = kNotSelected;  // decision variable

    bool isSelected() const { return selected == kSelected; }
};

// ---------------------------------------------------------------------------
// SelectionGroup  (problem fact)
// ---------------------------------------------------------------------------
// Between minSelected and maxSelected of `items` (distinct item indices) must
// be selected. No maxSelected = every item of the group may be.
struct SelectionGroup {
    std::string id;
    std::vector<int> items;
    int minSelected = 0;
    std::optional<int> maxSelected;

    int effectiveMax() const { return maxSelected ? *maxSelected : static_cast<int>(items.size()); }
};

// One (item, required item) pair, in items order and then requiredItems
// order: the term order of itemDependencyCost.
struct Dependency {
    int item;
    int required;
};

// ---------------------------------------------------------------------------
// KnapsackProblem  (solution)
// ---------------------------------------------------------------------------
struct KnapsackProblem {
    std::int64_t capacity = 0;  // maximum total selected weight
    std::vector<Item> items;
    std::vector<Resource> resources;
    // Item indices; an index may repeat (each entry counts once).
    std::vector<int> mandatoryItems;
    std::vector<int> forbiddenItems;
    std::vector<SelectionGroup> selectionGroups;

    // Start the next solve from the current selection (undecided items go
    // through the greedy construction); the solve resets it to false, so a
    // seed is used once.
    bool warmStart = false;

    // No constraint weights here: the defaults are the components'
    // kDefaultWeight, and a caller re-weights through a constraint assembly.

    Score score;

    // --- derived data, rebuilt by ScoreCalculator::prepare ----------------
    // Every entry point (solve, score, greedySelection, the Python
    // constraint_costs) re-runs prepare, so a problem edited after a solve is
    // never scored against stale indexes. The calculators read these.
    bool prepared = false;
    // item -> (declared resource index, amount), resources the item uses.
    std::vector<std::vector<std::pair<int, std::int64_t>>> itemResources;
    // item -> how often it is listed in mandatoryItems / forbiddenItems.
    std::vector<int> mandatoryCount;
    std::vector<int> forbiddenCount;
    // item -> the selection groups that list it.
    std::vector<std::vector<int>> itemGroups;
    // Every dependency pair (self-dependencies included: they cost 0).
    std::vector<Dependency> dependencies;
    // item -> the dependency indices where it is the item / the required
    // item. Self-dependencies are in neither, so a flip never re-evaluates a
    // pair twice.
    std::vector<std::vector<int>> dependenciesAsItem;
    std::vector<std::vector<int>> dependenciesAsRequired;

    int itemCount() const { return static_cast<int>(items.size()); }

    std::int64_t totalWeight() const {
        std::int64_t total = 0;
        for (const Item& item : items) {
            if (item.isSelected()) total += item.weight;
        }
        return total;
    }

    std::int64_t totalValue() const {
        std::int64_t total = 0;
        for (const Item& item : items) {
            if (item.isSelected()) total += item.value;
        }
        return total;
    }

    // The decision variable of every item, in item order (-1 / 0 / 1).
    std::vector<int> assignment() const {
        std::vector<int> out;
        out.reserve(items.size());
        for (const Item& item : items) out.push_back(item.selected);
        return out;
    }
};

}  // namespace ks

// ---------------------------------------------------------------------------
// SolutionTraits specialization: entities are the items, the decision
// variable is Item::selected, the value range is {0 = not selected,
// 1 = selected}. With two values the engine's change move is a flip and its
// swap move trades a selected item for an unselected one.
// ---------------------------------------------------------------------------
template <>
struct optimaru::SolutionTraits<ks::KnapsackProblem> {
    static constexpr optimaru::Shape shape = optimaru::Shape::Assignment;
    static int entity_count(const ks::KnapsackProblem& problem) { return problem.itemCount(); }
    static int value_count(const ks::KnapsackProblem& /*problem*/) { return 2; }
    static int value_of(const ks::KnapsackProblem& problem, int itemIndex) {
        return problem.items[itemIndex].selected;
    }
    static void assign(ks::KnapsackProblem& problem, int itemIndex, int value) {
        problem.items[itemIndex].selected = value;
    }
};

#endif  // KS_DOMAIN_HPP

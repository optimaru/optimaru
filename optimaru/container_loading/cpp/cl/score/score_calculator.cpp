#include "cl/score/score_calculator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cl/constraints.hpp"
#include "cl/score/packing.hpp"

namespace cl {

namespace {

void checkSize(const std::string& what, int length, int width, int height) {
    for (int side : {length, width, height}) {
        if (side <= 0 || side > kMaxLength) {
            throw std::invalid_argument(what + ": length, width and height must be between 1 and " +
                                        std::to_string(kMaxLength));
        }
    }
}

void validate(const ContainerLoadingProblem& problem) {
    if (!(problem.minSupportRatio > 0.0) || !(problem.minSupportRatio <= 1.0))
        throw std::invalid_argument("min_support_ratio must be above 0 and at most 1");
    if (problem.containers.empty() && !problem.items.empty())
        throw std::invalid_argument("a problem with items needs at least one container");
    std::unordered_set<std::string> ids;
    for (const Container& container : problem.containers) {
        if (!ids.insert(container.id).second)
            throw std::invalid_argument("container ids must be unique: '" + container.id + "'");
        const std::string what = "container '" + container.id + "'";
        checkSize(what, container.length, container.width, container.height);
        if (container.maxWeight && *container.maxWeight < 0)
            throw std::invalid_argument(what + ": max_weight must be >= 0");
        if (!std::isfinite(container.cost) || container.cost < 0.0)
            throw std::invalid_argument(what + ": cost must be finite and >= 0");
        if (container.maxStackHeight &&
            (*container.maxStackHeight <= 0 || *container.maxStackHeight > kMaxLength)) {
            throw std::invalid_argument(what + ": max_stack_height must be between 1 and " +
                                        std::to_string(kMaxLength));
        }
        for (const auto& [name, share] :
             {std::pair{"max_offset_length", container.maxOffsetLength},
              std::pair{"max_offset_width", container.maxOffsetWidth}}) {
            if (share && !(*share >= 0.0 && *share <= 1.0))
                throw std::invalid_argument(what + ": " + name + " must be between 0 and 1");
        }
    }
    ids.clear();
    for (const Item& item : problem.items) {
        if (!ids.insert(item.id).second)
            throw std::invalid_argument("item ids must be unique: '" + item.id + "'");
        const std::string what = "item '" + item.id + "'";
        checkSize(what, item.length, item.width, item.height);
        if (item.weight < 0 || item.weight > kMaxWeight) {
            throw std::invalid_argument(what + ": weight must be between 0 and " +
                                        std::to_string(kMaxWeight));
        }
        if (item.maxLoadOnTop && *item.maxLoadOnTop < 0)
            throw std::invalid_argument(what + ": max_load_on_top must be >= 0");
        if (item.maxStackLayers &&
            (*item.maxStackLayers < 1 || *item.maxStackLayers > kMaxLayers)) {
            throw std::invalid_argument(what + ": max_stack_layers must be between 1 and " +
                                        std::to_string(kMaxLayers));
        }
        if (item.priority < 1 || item.priority > kMaxPriority) {
            throw std::invalid_argument(what + ": priority must be between 1 and " +
                                        std::to_string(kMaxPriority));
        }
    }
    std::unordered_set<std::string> grouped;
    for (const Item& item : problem.items) {
        if (item.group) grouped.insert(*item.group);
    }
    ids.clear();
    for (const GroupRule& rule : problem.groupRules) {
        if (!ids.insert(rule.id).second)
            throw std::invalid_argument("group rule ids must be unique: '" + rule.id + "'");
        if (grouped.count(rule.id) == 0)
            throw std::invalid_argument("group rule '" + rule.id + "': no item has this group");
    }
    for (const StackingRule& rule : problem.stackingRules) {
        if (rule.top.empty() || rule.bottom.empty())
            throw std::invalid_argument("a stacking rule names an empty stack group");
    }
    std::vector<char> listed(problem.items.size(), 0);
    for (const Container& container : problem.containers) {
        for (int item : container.items) {
            if (item < 0 || item >= static_cast<int>(problem.items.size())) {
                throw std::invalid_argument("container '" + container.id +
                                            "': its list holds an item index out of range");
            }
            if (listed[static_cast<std::size_t>(item)]) {
                throw std::invalid_argument("item '" +
                                            problem.items[static_cast<std::size_t>(item)].id +
                                            "' is in more than one place of the loading lists");
            }
            listed[static_cast<std::size_t>(item)] = 1;
        }
    }
}

// The orientations the decoder may use for an item, in its order of
// preference among those that divide the free room equally well (decode in
// cl/score/packing.cpp): the lowest first (the largest base, which rests best
// and leaves the most to stand on), and of two equally low ones the one that
// reaches less far towards the door. `Upright` and `Fixed` keep the height as
// given.
void appendOrientations(const Item& item, std::vector<Orientation>& out) {
    const std::size_t first = out.size();
    const std::array<int, 3> side = {item.length, item.width, item.height};
    static constexpr int kPermutations[6][3] = {{0, 1, 2}, {1, 0, 2}, {0, 2, 1},
                                                {2, 0, 1}, {1, 2, 0}, {2, 1, 0}};
    const int count = item.rotations == Rotation::All       ? 6
                      : item.rotations == Rotation::Upright ? 2
                                                            : 1;
    for (int p = 0; p < count; ++p) {
        const Orientation candidate{side[static_cast<std::size_t>(kPermutations[p][0])],
                                    side[static_cast<std::size_t>(kPermutations[p][1])],
                                    side[static_cast<std::size_t>(kPermutations[p][2])]};
        bool known = false;
        for (std::size_t i = first; i < out.size(); ++i) {
            known = known || (out[i].length == candidate.length &&
                              out[i].width == candidate.width && out[i].height == candidate.height);
        }
        if (!known) out.push_back(candidate);
    }
    std::stable_sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(),
                     [](const Orientation& a, const Orientation& b) {
                         if (a.height != b.height) return a.height < b.height;
                         return a.length < b.length;
                     });
}

// Per flagged group (all-or-none ones when `allOrNone`, else the
// single-container ones), how many of its items a container's list holds and
// how many of those found a place. Written apart from
// PlacedGroupConstraint::tally on purpose: this full recompute is what the
// incremental component is checked against.
void groupTally(const ContainerLoadingProblem& problem, const std::vector<int>& members,
                const Packing& packing, bool allOrNone, std::vector<detail::GroupTally>& out) {
    out.clear();
    const std::vector<char>& flags =
            allOrNone ? problem.groupAllOrNone : problem.groupSingleContainer;
    for (std::size_t position = 0; position < members.size(); ++position) {
        const int group = problem.groupOf[static_cast<std::size_t>(members[position])];
        if (group < 0 || !flags[static_cast<std::size_t>(group)]) continue;
        auto found = std::find_if(out.begin(), out.end(), [group](const detail::GroupTally& t) {
            return t.group == group;
        });
        if (found == out.end()) {
            out.push_back({group, 0, 0});
            found = out.end() - 1;
        }
        ++found->listed;
        if (packing.boxOf[position] >= 0) ++found->placed;
    }
}

}  // namespace

Score ConstraintTotals::toScore() const {
    const double hard =
            UnplacedItemCost::kDefaultWeight * static_cast<double>(unplacedItemCost) +
            ContainerWeightCost::kDefaultWeight * static_cast<double>(containerWeightCost) +
            StackingLoadCost::kDefaultWeight * static_cast<double>(stackingLoadCost) +
            StackingRuleCost::kDefaultWeight * static_cast<double>(stackingRuleCost) +
            UnloadingOrderCost::kDefaultWeight * static_cast<double>(unloadingOrderCost) +
            BalanceLimitCost::kDefaultWeight * static_cast<double>(balanceLimitCost) +
            IncompleteGroupCost::kDefaultWeight * static_cast<double>(incompleteGroupCost) +
            SingleContainerCost::kDefaultWeight * static_cast<double>(singleContainerCost);
    const double soft = UsedContainerCost::kDefaultWeight * usedContainerCost +
                        OptionalItemCost::kDefaultWeight * static_cast<double>(optionalItemCost) +
                        GroupSplitCost::kDefaultWeight * static_cast<double>(groupSplitCost) +
                        UsedLengthCost::kDefaultWeight * static_cast<double>(usedLengthCost) +
                        LoadBalanceCost::kDefaultWeight * static_cast<double>(loadBalanceCost);
    return {-static_cast<std::int64_t>(std::llround(hard)), -soft};
}

void ScoreCalculator::prepare(ContainerLoadingProblem& problem) {
    validate(problem);
    const std::size_t count = problem.items.size();
    problem.orientations.clear();
    problem.orientationStart.assign(1, 0);
    problem.smallestSide.resize(count);
    problem.groupOf.assign(count, -1);
    problem.groupCount = 0;
    problem.anyLoadLimit = false;
    problem.anyUnloadOrder = false;
    problem.anyLayerLimit = false;
    problem.anyOptional = false;
    problem.anyPriority = false;
    for (Container& container : problem.containers) {
        container.fillHeight = container.maxStackHeight
                                       ? std::min(*container.maxStackHeight, container.height)
                                       : container.height;
    }
    // The stack groups the rules name, numbered densely; an item's group that
    // no rule names is -1, as no group.
    std::unordered_map<std::string, int> stackGroups;
    for (const StackingRule& rule : problem.stackingRules) {
        for (const std::string* name : {&rule.top, &rule.bottom})
            stackGroups.try_emplace(*name, static_cast<int>(stackGroups.size()));
    }
    problem.stackGroupCount = static_cast<int>(stackGroups.size());
    problem.forbidden.assign(static_cast<std::size_t>(problem.stackGroupCount) *
                                     static_cast<std::size_t>(problem.stackGroupCount),
                             0);
    for (const StackingRule& rule : problem.stackingRules) {
        problem.forbidden[static_cast<std::size_t>(stackGroups.at(rule.top)) *
                                  static_cast<std::size_t>(problem.stackGroupCount) +
                          static_cast<std::size_t>(stackGroups.at(rule.bottom))] = 1;
    }
    problem.stackGroupOf.assign(count, -1);
    problem.anyStackRule = false;
    std::unordered_map<std::string, int> groups;
    for (std::size_t i = 0; i < count; ++i) {
        const Item& item = problem.items[i];
        appendOrientations(item, problem.orientations);
        problem.orientationStart.push_back(static_cast<int>(problem.orientations.size()));
        problem.smallestSide[i] = std::min({item.length, item.width, item.height});
        if (item.group) {
            const auto [at, added] = groups.try_emplace(*item.group, problem.groupCount);
            if (added) ++problem.groupCount;
            problem.groupOf[i] = at->second;
        }
        problem.anyLoadLimit = problem.anyLoadLimit || item.maxLoadOnTop.has_value();
        problem.anyUnloadOrder = problem.anyUnloadOrder || item.unloadOrder.has_value();
        problem.anyLayerLimit = problem.anyLayerLimit || item.maxStackLayers.has_value();
        problem.anyOptional = problem.anyOptional || item.optional;
        problem.anyPriority = problem.anyPriority || item.priority != 1;
        if (item.stackGroup && problem.stackGroupCount > 0) {
            const auto found = stackGroups.find(*item.stackGroup);
            if (found != stackGroups.end()) {
                problem.stackGroupOf[i] = found->second;
                problem.anyStackRule = true;
            }
        }
    }
    problem.groupAllOrNone.assign(static_cast<std::size_t>(problem.groupCount), 0);
    problem.groupSingleContainer.assign(static_cast<std::size_t>(problem.groupCount), 0);
    problem.anyAllOrNone = false;
    problem.anySingleContainer = false;
    for (const GroupRule& rule : problem.groupRules) {
        const auto g = static_cast<std::size_t>(groups.at(rule.id));
        problem.groupAllOrNone[g] = rule.allOrNone ? 1 : 0;
        problem.groupSingleContainer[g] = rule.singleContainer ? 1 : 0;
        problem.anyAllOrNone = problem.anyAllOrNone || rule.allOrNone;
        problem.anySingleContainer = problem.anySingleContainer || rule.singleContainer;
    }
}

void ScoreCalculator::updateDerived(ContainerLoadingProblem& problem) {
    for (Item& item : problem.items) {
        item.container = -1;
        item.sequence = -1;
        item.x = item.y = item.z = 0;
        item.placedLength = item.placedWidth = item.placedHeight = 0;
    }
    Packing packing;
    for (std::size_t c = 0; c < problem.containers.size(); ++c) {
        Container& container = problem.containers[c];
        decode(problem, static_cast<int>(c), container.items, packing);
        container.placedItems.clear();
        container.unplacedItems.clear();
        for (std::size_t position = 0; position < container.items.size(); ++position) {
            if (packing.boxOf[position] < 0)
                container.unplacedItems.push_back(container.items[position]);
        }
        for (const Box& box : packing.boxes) {
            Item& item = problem.items[static_cast<std::size_t>(box.item)];
            item.container = static_cast<int>(c);
            item.sequence = static_cast<int>(container.placedItems.size());
            item.x = box.x;
            item.y = box.y;
            item.z = box.z;
            item.placedLength = box.length;
            item.placedWidth = box.width;
            item.placedHeight = box.height;
            container.placedItems.push_back(box.item);
        }
        container.loadedWeight = packing.loadedWeight;
        container.loadedVolume = packing.loadedVolume;
        container.usedLength = packing.usedLength;
    }
}

ConstraintTotals ScoreCalculator::calculateTotals(ContainerLoadingProblem& problem) {
    prepare(problem);
    ConstraintTotals totals;
    Packing packing;
    const IncompleteGroupCost incomplete;
    const SingleContainerCost single;
    std::vector<std::vector<detail::GroupTally>> allOrNone(problem.containers.size());
    std::vector<std::vector<detail::GroupTally>> together(problem.containers.size());
    for (std::size_t c = 0; c < problem.containers.size(); ++c) {
        const Container& container = problem.containers[c];
        const int unit = static_cast<int>(c);
        if (container.items.empty()) continue;
        decode(problem, unit, container.items, packing);
        totals.containerWeightCost += ContainerWeightCost::excess(
                problem, unit, container.items, unplacedOptionalWeight(problem, packing));
        totals.unplacedItemCost += unplacedCost(problem, packing);
        totals.optionalItemCost += optionalCost(problem, packing);
        if (problem.anyAllOrNone) groupTally(problem, container.items, packing, true, allOrNone[c]);
        if (problem.anySingleContainer)
            groupTally(problem, container.items, packing, false, together[c]);
        totals.stackingLoadCost += stackingExcess(problem, packing);
        totals.stackingRuleCost += stackingViolations(problem, packing);
        totals.unloadingOrderCost += unloadingBlocks(problem, packing);
        totals.balanceLimitCost += balanceExcess(problem, unit, packing);
        if (!packing.boxes.empty()) totals.usedContainerCost += container.cost;
        totals.usedLengthCost += packing.usedLength;
        totals.loadBalanceCost += balancePerMille(problem, unit, packing);
    }
    totals.groupSplitCost = GroupSplitCost::splits(problem);
    if (problem.anyAllOrNone) totals.incompleteGroupCost = incomplete.totalOf(problem, allOrNone);
    if (problem.anySingleContainer) totals.singleContainerCost = single.totalOf(problem, together);
    return totals;
}

Score ScoreCalculator::score(ContainerLoadingProblem& problem) {
    const ConstraintTotals totals = calculateTotals(problem);
    updateDerived(problem);
    problem.score = totals.toScore();
    return problem.score;
}

}  // namespace cl

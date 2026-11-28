#include "cl/score/score_calculator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

}  // namespace

Score ConstraintTotals::toScore() const {
    const double hard =
            UnplacedItemCost::kDefaultWeight * static_cast<double>(unplacedItemCost) +
            ContainerWeightCost::kDefaultWeight * static_cast<double>(containerWeightCost) +
            StackingLoadCost::kDefaultWeight * static_cast<double>(stackingLoadCost) +
            UnloadingOrderCost::kDefaultWeight * static_cast<double>(unloadingOrderCost);
    const double soft = UsedContainerCost::kDefaultWeight * usedContainerCost +
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
    for (std::size_t c = 0; c < problem.containers.size(); ++c) {
        const Container& container = problem.containers[c];
        const int unit = static_cast<int>(c);
        totals.containerWeightCost += ContainerWeightCost::excess(problem, unit, container.items);
        if (container.items.empty()) continue;
        decode(problem, unit, container.items, packing);
        totals.unplacedItemCost += packing.unplaced();
        totals.stackingLoadCost += stackingExcess(problem, packing);
        totals.unloadingOrderCost += unloadingBlocks(problem, packing);
        if (!packing.boxes.empty()) totals.usedContainerCost += container.cost;
        totals.usedLengthCost += packing.usedLength;
        totals.loadBalanceCost += balancePerMille(problem, unit, packing);
    }
    totals.groupSplitCost = GroupSplitCost::splits(problem);
    return totals;
}

Score ScoreCalculator::score(ContainerLoadingProblem& problem) {
    const ConstraintTotals totals = calculateTotals(problem);
    updateDerived(problem);
    problem.score = totals.toScore();
    return problem.score;
}

}  // namespace cl

#include "cl/construct/construction.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "cl/score/packing.hpp"

namespace cl {

int insertPosition(const ContainerLoadingProblem& problem, const std::vector<int>& list, int item) {
    const auto& order = problem.items[static_cast<std::size_t>(item)].unloadOrder;
    if (!order) return static_cast<int>(list.size());
    for (std::size_t position = 0; position < list.size(); ++position) {
        const auto& other = problem.items[static_cast<std::size_t>(list[position])].unloadOrder;
        if (other && *other < *order) return static_cast<int>(position);
    }
    return static_cast<int>(list.size());
}

namespace {

// No unloading order sorts with the latest: nothing has to wait for it.
int unloadRank(const Item& item) {
    return item.unloadOrder ? *item.unloadOrder : std::numeric_limits<int>::max();
}

}  // namespace

bool loadsBefore(const ContainerLoadingProblem& problem, int a, int b) {
    const Item& first = problem.items[static_cast<std::size_t>(a)];
    const Item& second = problem.items[static_cast<std::size_t>(b)];
    if (unloadRank(first) != unloadRank(second)) return unloadRank(first) > unloadRank(second);
    if (first.bottomOnly != second.bottomOnly) return first.bottomOnly;
    if (first.maxLoadOnTop.has_value() != second.maxLoadOnTop.has_value())
        return !first.maxLoadOnTop.has_value();
    if (first.volume() != second.volume()) return first.volume() > second.volume();
    return static_cast<std::int64_t>(first.length) * first.width >
           static_cast<std::int64_t>(second.length) * second.width;
}

void constructLoading(ContainerLoadingProblem& problem) {
    const std::size_t itemCount = problem.items.size();
    const std::size_t containerCount = problem.containers.size();
    if (itemCount == 0 || containerCount == 0) return;

    std::vector<char> listed(itemCount, 0);
    for (const Container& container : problem.containers) {
        for (int item : container.items) listed[static_cast<std::size_t>(item)] = 1;
    }
    std::vector<int> pending;
    for (std::size_t i = 0; i < itemCount; ++i) {
        if (!listed[i]) pending.push_back(static_cast<int>(i));
    }
    if (pending.empty()) return;
    // A group's items go one after the other, the group with the most volume
    // first: what belongs together then fills the same containers. An item
    // without a group stands for itself.
    // Summed in double: a group's volume is not bounded by a container's, and
    // an int64 sum of items near the size limits could overflow.
    std::vector<double> groupVolume(static_cast<std::size_t>(problem.groupCount), 0.0);
    for (int item : pending) {
        const int group = problem.groupOf[static_cast<std::size_t>(item)];
        if (group >= 0)
            groupVolume[static_cast<std::size_t>(group)] +=
                    static_cast<double>(problem.items[static_cast<std::size_t>(item)].volume());
    }
    const auto together = [&problem, &groupVolume](int item) {
        const int group = problem.groupOf[static_cast<std::size_t>(item)];
        return group >= 0 ? groupVolume[static_cast<std::size_t>(group)]
                          : static_cast<double>(
                                    problem.items[static_cast<std::size_t>(item)].volume());
    };
    std::stable_sort(pending.begin(), pending.end(), [&problem, &together](int a, int b) {
        const Item& first = problem.items[static_cast<std::size_t>(a)];
        const Item& second = problem.items[static_cast<std::size_t>(b)];
        if (unloadRank(first) != unloadRank(second)) return unloadRank(first) > unloadRank(second);
        if (first.bottomOnly != second.bottomOnly) return first.bottomOnly;
        if (first.maxLoadOnTop.has_value() != second.maxLoadOnTop.has_value())
            return !first.maxLoadOnTop.has_value();
        if (together(a) != together(b)) return together(a) > together(b);
        const int groupA = problem.groupOf[static_cast<std::size_t>(a)];
        const int groupB = problem.groupOf[static_cast<std::size_t>(b)];
        if (groupA != groupB) return groupA < groupB;
        return loadsBefore(problem, a, b);
    });

    PackingCache cache;
    cache.reset(problem);
    std::vector<std::int64_t> weight(containerCount, 0);
    std::vector<std::int64_t> volume(containerCount, 0);  // of the placed items
    std::vector<int> unplaced(containerCount, 0);
    std::vector<std::int64_t> excess(containerCount, 0);  // stacking excess
    std::vector<std::int64_t> blocks(containerCount, 0);  // unloading blocks
    std::vector<std::int64_t> rules(containerCount, 0);   // stacking rule violations
    for (std::size_t c = 0; c < containerCount; ++c) {
        const Container& container = problem.containers[c];
        for (int item : container.items)
            weight[c] += problem.items[static_cast<std::size_t>(item)].weight;
        if (container.items.empty()) continue;
        const Packing& packing = cache.packing(problem, static_cast<int>(c), container.items);
        unplaced[c] = packing.unplaced();
        volume[c] = packing.loadedVolume;
        excess[c] = stackingExcess(problem, packing);
        blocks[c] = unloadingBlocks(problem, packing);
        rules[c] = stackingViolations(problem, packing);
    }
    // The empty containers, cheapest per volume first.
    std::vector<int> byPrice(containerCount);
    for (std::size_t c = 0; c < containerCount; ++c) byPrice[c] = static_cast<int>(c);
    std::stable_sort(byPrice.begin(), byPrice.end(), [&problem](int a, int b) {
        const Container& first = problem.containers[static_cast<std::size_t>(a)];
        const Container& second = problem.containers[static_cast<std::size_t>(b)];
        return first.cost / static_cast<double>(first.volume()) <
               second.cost / static_cast<double>(second.volume());
    });

    std::vector<int> candidates;
    std::vector<int> trial;
    for (int item : pending) {
        const Item& box = problem.items[static_cast<std::size_t>(item)];
        const int group = problem.groupOf[static_cast<std::size_t>(item)];
        candidates.clear();
        if (group >= 0) {
            for (std::size_t c = 0; c < containerCount; ++c) {
                const std::vector<int>& list = problem.containers[c].items;
                const bool holds = std::any_of(list.begin(), list.end(), [&](int other) {
                    return problem.groupOf[static_cast<std::size_t>(other)] == group;
                });
                if (holds) candidates.push_back(static_cast<int>(c));
            }
        }
        const std::size_t sameGroup = candidates.size();
        for (std::size_t c = 0; c < containerCount; ++c) {
            if (problem.containers[c].items.empty()) continue;
            if (std::find(candidates.begin(),
                          candidates.begin() + static_cast<std::ptrdiff_t>(sameGroup),
                          static_cast<int>(c)) ==
                candidates.begin() + static_cast<std::ptrdiff_t>(sameGroup))
                candidates.push_back(static_cast<int>(c));
        }
        for (int c : byPrice) {
            if (problem.containers[static_cast<std::size_t>(c)].items.empty())
                candidates.push_back(c);
        }

        int chosen = -1;
        for (int c : candidates) {
            const auto index = static_cast<std::size_t>(c);
            const Container& container = problem.containers[index];
            if (container.maxWeight && weight[index] + box.weight > *container.maxWeight) continue;
            if (volume[index] + box.volume() > container.volume()) continue;
            trial = container.items;
            trial.insert(trial.begin() + insertPosition(problem, trial, item), item);
            const Packing& packing = cache.packing(problem, c, trial);
            if (packing.unplaced() > unplaced[index]) continue;
            // Each kind of violation on its own: no trading one for the other.
            if (stackingExcess(problem, packing) > excess[index]) continue;
            if (unloadingBlocks(problem, packing) > blocks[index]) continue;
            if (stackingViolations(problem, packing) > rules[index]) continue;
            chosen = c;
            break;
        }
        if (chosen < 0) {
            std::int64_t mostFree = -1;
            for (std::size_t c = 0; c < containerCount; ++c) {
                const std::int64_t free = problem.containers[c].volume() - volume[c];
                if (free > mostFree) {
                    mostFree = free;
                    chosen = static_cast<int>(c);
                }
            }
        }
        const auto home = static_cast<std::size_t>(chosen);
        std::vector<int>& list = problem.containers[home].items;
        list.insert(list.begin() + insertPosition(problem, list, item), item);
        const Packing& packing = cache.packing(problem, chosen, list);
        weight[home] += box.weight;
        volume[home] = packing.loadedVolume;
        unplaced[home] = packing.unplaced();
        excess[home] = stackingExcess(problem, packing);
        blocks[home] = unloadingBlocks(problem, packing);
        rules[home] = stackingViolations(problem, packing);
    }
}

}  // namespace cl

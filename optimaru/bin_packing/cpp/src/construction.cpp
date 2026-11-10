#include "bp/construction.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

namespace bp {

std::vector<int> descendingOrder(const BinPackingProblem& problem) {
    std::vector<int> order(problem.items.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&problem](int a, int b) {
        return problem.items[a].weight > problem.items[b].weight;
    });
    return order;
}

std::vector<int> tieShuffledOrder(const BinPackingProblem& problem, std::mt19937_64& rng) {
    std::vector<int> order = descendingOrder(problem);
    for (std::size_t begin = 0; begin < order.size();) {
        std::size_t end = begin + 1;
        while (end < order.size() &&
               problem.items[order[end]].weight == problem.items[order[begin]].weight) {
            ++end;
        }
        std::shuffle(order.begin() + static_cast<std::ptrdiff_t>(begin),
                     order.begin() + static_cast<std::ptrdiff_t>(end), rng);
        begin = end;
    }
    return order;
}

void constructBins(BinPackingProblem& problem, const std::vector<int>& order) {
    const int n = problem.itemCount();
    const std::int64_t capacity = problem.binCapacity;
    int top = -1;
    for (const Item& item : problem.items) top = std::max(top, item.bin);
    std::vector<std::int64_t> loads(static_cast<std::size_t>(top + 1), 0);
    for (const Item& item : problem.items) {
        if (item.bin >= 0) loads[item.bin] += item.weight;
    }
    for (int index : order) {
        Item& item = problem.items[index];
        if (item.bin >= 0) continue;
        const std::int64_t weight = item.weight;
        int selected = -1;
        std::int64_t bestRoom = 0;
        for (std::size_t bin = 0; bin < loads.size(); ++bin) {
            if (loads[bin] + weight > capacity) continue;
            const std::int64_t room = capacity - (loads[bin] + weight);
            if (selected < 0 || room < bestRoom) {
                selected = static_cast<int>(bin);
                bestRoom = room;
            }
        }
        if (selected < 0) {
            if (static_cast<int>(loads.size()) < n) {
                selected = static_cast<int>(loads.size());
                loads.push_back(0);
            } else {
                // Fewer than n items are placed while this one is not, so an
                // empty bin exists (weights are > 0: empty == load 0).
                selected =
                        static_cast<int>(std::find(loads.begin(), loads.end(), 0) - loads.begin());
            }
        }
        item.bin = selected;
        loads[selected] += weight;
    }
}

void compactBins(BinPackingProblem& problem) {
    std::vector<int> remap(problem.items.size(), -1);
    int next = 0;
    for (Item& item : problem.items) {
        if (item.bin < 0) continue;
        if (remap[item.bin] < 0) remap[item.bin] = next++;
        item.bin = remap[item.bin];
    }
}

}  // namespace bp

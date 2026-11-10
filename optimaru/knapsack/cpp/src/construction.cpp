#include "ks/construction.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace ks {

std::vector<int> greedySelection(const KnapsackProblem& problem, const std::vector<int>& fixed) {
    const int n = problem.itemCount();
    if (static_cast<int>(fixed.size()) != n) {
        throw std::invalid_argument("fixed must hold one entry per item (" + std::to_string(n) +
                                    "), not " + std::to_string(fixed.size()));
    }
    for (int i = 0; i < n; ++i) {
        if (fixed[i] < kUndecided || fixed[i] > kSelected) {
            throw std::invalid_argument("item '" + problem.items[i].id +
                                        "': fixed must be -1, 0 or 1, not " +
                                        std::to_string(fixed[i]));
        }
    }

    // The density as Python computed it (value / weight as a double); a
    // stable sort keeps equal densities in item order, like sorted(...,
    // reverse=True).
    std::vector<double> density(n);
    for (int i = 0; i < n; ++i) {
        density[i] = static_cast<double>(problem.items[i].value) /
                     static_cast<double>(problem.items[i].weight);
    }
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&density](int a, int b) { return density[a] > density[b]; });

    std::vector<int> selection(n, kNotSelected);
    std::int64_t load = 0;
    for (int i = 0; i < n; ++i) {
        if (fixed[i] == kSelected) {
            selection[i] = kSelected;
            load += problem.items[i].weight;
        }
    }
    for (int i : order) {
        if (fixed[i] == kUndecided && load + problem.items[i].weight <= problem.capacity) {
            selection[i] = kSelected;
            load += problem.items[i].weight;
        }
    }
    return selection;
}

void constructSelection(KnapsackProblem& problem) {
    std::vector<int> fixed(problem.items.size(), kUndecided);
    if (problem.warmStart) fixed = problem.assignment();
    const std::vector<int> selection = greedySelection(problem, fixed);
    for (std::size_t i = 0; i < selection.size(); ++i) problem.items[i].selected = selection[i];
}

}  // namespace ks

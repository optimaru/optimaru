// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_CONSTRUCT_INITIALIZATION_ORDER_HPP
#define OPTIMARU_CONSTRUCT_INITIALIZATION_ORDER_HPP

#include <algorithm>
#include <functional>
#include <vector>

namespace optimaru {

// The order a construction places its candidates in: decreasing difficulty
// when ConstructionSettings::difficulty is set (first fit decreasing),
// otherwise index order (first fit). Ties keep index order.
inline std::vector<int> initialization_order(const std::vector<int>& candidates,
                                             const std::function<double(int)>& difficulty) {
    std::vector<int> order = candidates;
    if (difficulty) {
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b) { return difficulty(a) > difficulty(b); });
    }
    return order;
}

}  // namespace optimaru

#endif  // OPTIMARU_CONSTRUCT_INITIALIZATION_ORDER_HPP

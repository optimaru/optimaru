// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_MODEL_NEIGHBORHOODS_HPP
#define OPTIMARU_MODEL_NEIGHBORHOODS_HPP

#include <algorithm>
#include <functional>
#include <vector>

namespace optimaru {

// ---------------------------------------------------------------------------
// Neighborhoods
// ---------------------------------------------------------------------------
// Precomputed k-nearest-neighbor lists for nearby selection (a
// domain-provided distance). Built once at solve start from the domain-provided
// Settings::search.nearby.distance; used to bias move selection toward pairs
// that are close to each other (a distant relocate/swap is almost never an
// improvement on spatial problems) and to drive the LNS related-ruin operator.
class Neighborhoods {
public:
    void build(int n, int k, const std::function<double(int, int)>& meter) {
        neighbors_.assign(n, {});
        if (n < 2) return;
        std::vector<std::pair<double, int>> by_distance;
        for (int i = 0; i < n; ++i) {
            by_distance.clear();
            for (int j = 0; j < n; ++j) {
                if (j != i) by_distance.emplace_back(meter(i, j), j);
            }
            int keep = std::min<int>(k, static_cast<int>(by_distance.size()));
            std::partial_sort(by_distance.begin(), by_distance.begin() + keep, by_distance.end());
            neighbors_[i].reserve(keep);
            for (int p = 0; p < keep; ++p) neighbors_[i].push_back(by_distance[p].second);
        }
    }

    bool enabled() const { return !neighbors_.empty(); }
    const std::vector<int>& of(int i) const { return neighbors_[i]; }

private:
    std::vector<std::vector<int>> neighbors_;  // ascending by distance
};

}  // namespace optimaru

#endif  // OPTIMARU_MODEL_NEIGHBORHOODS_HPP

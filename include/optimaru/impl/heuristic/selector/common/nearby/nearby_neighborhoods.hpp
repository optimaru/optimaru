#ifndef OPTIMARU_IMPL_HEURISTIC_SELECTOR_COMMON_NEARBY_NEARBY_NEIGHBORHOODS_HPP
#define OPTIMARU_IMPL_HEURISTIC_SELECTOR_COMMON_NEARBY_NEARBY_NEIGHBORHOODS_HPP

#include <algorithm>
#include <functional>
#include <vector>

namespace optimaru {

// ---------------------------------------------------------------------------
// NearbyNeighborhoods
// ---------------------------------------------------------------------------
// Precomputed k-nearest-neighbor lists for nearby selection (a
// domain-provided distance meter). Built once at solve start from the domain-provided
// SolverConfig::nearbyDistanceMeter; used to bias move selection toward pairs
// that are close to each other (a distant relocate/swap is almost never an
// improvement on spatial problems) and to drive the LNS related-ruin operator.
class NearbyNeighborhoods {
public:
    void build(int n, int k, const std::function<double(int, int)>& meter) {
        neighbors_.assign(n, {});
        if (n < 2) return;
        std::vector<std::pair<double, int>> byDistance;
        for (int i = 0; i < n; ++i) {
            byDistance.clear();
            for (int j = 0; j < n; ++j) {
                if (j != i) byDistance.emplace_back(meter(i, j), j);
            }
            int keep = std::min<int>(k, static_cast<int>(byDistance.size()));
            std::partial_sort(byDistance.begin(), byDistance.begin() + keep, byDistance.end());
            neighbors_[i].reserve(keep);
            for (int p = 0; p < keep; ++p) neighbors_[i].push_back(byDistance[p].second);
        }
    }

    bool enabled() const { return !neighbors_.empty(); }
    const std::vector<int>& of(int i) const { return neighbors_[i]; }

private:
    std::vector<std::vector<int>> neighbors_;  // ascending by distance
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_HEURISTIC_SELECTOR_COMMON_NEARBY_NEARBY_NEIGHBORHOODS_HPP

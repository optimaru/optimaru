#ifndef OPTIMARU_CONFIG_HEURISTIC_SELECTOR_COMMON_NEARBY_NEARBY_SELECTION_CONFIG_HPP
#define OPTIMARU_CONFIG_HEURISTIC_SELECTOR_COMMON_NEARBY_NEARBY_SELECTION_CONFIG_HPP

#include <functional>

namespace optimaru {

// Mirrors config.heuristic.selector.common.nearby.NearbySelectionConfig.
// When distanceMeter is set (the NearbyDistanceMeter analog: domain distance
// between two entities (BASIC) or elements (LIST)), the engine precomputes
// size-nearest-neighbor lists and biases swap partners / relocate targets
// toward them with the given probability. The same lists drive the LNS
// related ruin and the nearby-restricted repair.
struct NearbySelectionConfig {
    std::function<double(int, int)> distanceMeter;
    int size = 20;
    double probability = 0.8;
};

}  // namespace optimaru

#endif  // OPTIMARU_CONFIG_HEURISTIC_SELECTOR_COMMON_NEARBY_NEARBY_SELECTION_CONFIG_HPP

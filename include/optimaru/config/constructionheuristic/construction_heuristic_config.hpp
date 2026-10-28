#ifndef OPTIMARU_CONFIG_CONSTRUCTIONHEURISTIC_CONSTRUCTION_HEURISTIC_CONFIG_HPP
#define OPTIMARU_CONFIG_CONSTRUCTIONHEURISTIC_CONSTRUCTION_HEURISTIC_CONFIG_HPP

#include <functional>

namespace optimaru {

// Mirrors config.constructionheuristic.ConstructionHeuristicPhaseConfig.
// If entityDifficulty is set, entities/elements are initialized in DECREASING
// difficulty order (First Fit Decreasing); otherwise index order (First Fit).
struct ConstructionHeuristicConfig {
    std::function<double(int entityIndex)> entityDifficulty;
};

}  // namespace optimaru

#endif  // OPTIMARU_CONFIG_CONSTRUCTIONHEURISTIC_CONSTRUCTION_HEURISTIC_CONFIG_HPP

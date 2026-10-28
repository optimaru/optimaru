#ifndef OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_RUIN_RECREATE_SELECTOR_HPP
#define OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_RUIN_RECREATE_SELECTOR_HPP

// ---------------------------------------------------------------------------
// Adaptive operator selection — umbrella header
// ---------------------------------------------------------------------------
// The plugin point lives in select/operator_selection_scheme.hpp and one
// scheme per file next to it. Including this header
// still gives the whole set plus the factory, so call sites do not change.
//
// Adding a scheme is a file there, a line here, an enum value and a case in
// the factory below.

#include <memory>

#include "optimaru/config/localsearch/local_search_config.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/select/alpha_ucb.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/select/operator_selection_scheme.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/select/random_select.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/select/roulette_wheel.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/select/segmented_roulette_wheel.hpp"

namespace optimaru {

inline std::unique_ptr<OperatorSelectionScheme> makeOperatorSelectionScheme(
        OperatorSelectionType type, int operatorCount, const std::vector<double>& rewards,
        double decay, std::int64_t segmentLength, double alpha) {
    switch (type) {
        case OperatorSelectionType::SEGMENTED_ROULETTE_WHEEL:
            return std::make_unique<SegmentedRouletteWheelSelector>(operatorCount, rewards, decay,
                                                                    segmentLength);
        case OperatorSelectionType::ALPHA_UCB:
            return std::make_unique<AlphaUcbSelector>(operatorCount, rewards, alpha);
        case OperatorSelectionType::RANDOM:
            return std::make_unique<RandomOperatorSelector>(operatorCount);
        case OperatorSelectionType::ROULETTE_WHEEL:
        default:
            return std::make_unique<RouletteWheelSelector>(operatorCount, rewards, decay);
    }
}



  // OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_RUIN_RECREATE_SELECTOR_HPP

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_RUIN_RECREATE_SELECTOR_HPP

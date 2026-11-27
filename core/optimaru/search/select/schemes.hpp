// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_SELECT_SCHEMES_HPP
#define OPTIMARU_SEARCH_SELECT_SCHEMES_HPP

// ---------------------------------------------------------------------------
// Adaptive operator selection — umbrella header
// ---------------------------------------------------------------------------
// The plugin point lives in select/scheme.hpp, with one scheme per file
// next to it. This header gives the whole set plus the factory.
//
// Adding a scheme is a file there, a line here, an enum value and a case in
// the factory below.

#include <memory>

#include "optimaru/settings.hpp"
#include "optimaru/search/select/alpha_ucb.hpp"
#include "optimaru/search/select/scheme.hpp"
#include "optimaru/search/select/uniform.hpp"
#include "optimaru/search/select/roulette_wheel.hpp"
#include "optimaru/search/select/segmented_roulette_wheel.hpp"

namespace optimaru {

// Builds the configured scheme. `segment_length` applies to
// SegmentedRouletteWheel, `alpha` to AlphaUcb; both are ignored otherwise.
inline std::unique_ptr<OperatorSelectionScheme> make_operator_selection_scheme(
        SelectionScheme type, int operator_count, const std::vector<double>& rewards, double decay,
        std::int64_t segment_length, double alpha) {
    switch (type) {
        case SelectionScheme::SegmentedRouletteWheel:
            return std::make_unique<SegmentedRouletteWheelSelector>(operator_count, rewards, decay,
                                                                    segment_length);
        case SelectionScheme::AlphaUcb:
            return std::make_unique<AlphaUcbSelector>(operator_count, rewards, alpha);
        case SelectionScheme::Uniform:
            return std::make_unique<RandomOperatorSelector>(operator_count);
        case SelectionScheme::RouletteWheel:
        default:
            return std::make_unique<RouletteWheelSelector>(operator_count, rewards, decay);
    }
}

// The scheme the ALNS settings configure, over `operator_count` operators.
inline std::unique_ptr<OperatorSelectionScheme> make_operator_selection_scheme(
        const AlnsSettings& lns, std::size_t operator_count) {
    return make_operator_selection_scheme(lns.selection_type, static_cast<int>(operator_count),
                                          lns.rewards, lns.operator_decay,
                                          lns.selection_segment_length, lns.selection_alpha);
}

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_SELECT_SCHEMES_HPP

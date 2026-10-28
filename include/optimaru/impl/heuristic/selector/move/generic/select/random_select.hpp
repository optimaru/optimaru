#ifndef OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_SELECT_RANDOM_SELECT_HPP
#define OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_SELECT_RANDOM_SELECT_HPP

#include <random>

#include "optimaru/impl/heuristic/selector/move/generic/select/operator_selection_scheme.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// RandomOperatorSelector
// ---------------------------------------------------------------------------
// Uniform selection, ignoring every outcome. The non-adaptive control: the
// baseline an adaptive scheme has to beat before its bookkeeping is worth it.
class RandomOperatorSelector final : public OperatorSelectionScheme {
public:
    explicit RandomOperatorSelector(int operatorCount) : OperatorSelectionScheme(operatorCount) {}

    int select(std::mt19937_64& rng) override {
        return static_cast<int>(rng() % static_cast<std::uint64_t>(operatorCount_));
    }
    void update(int, LnsOutcome) override {}
};

// Builds the configured scheme. `segmentLength` applies to
// SEGMENTED_ROULETTE_WHEEL, `alpha` to ALPHA_UCB; both are ignored otherwise.

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_SELECT_RANDOM_SELECT_HPP

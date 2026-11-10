// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SELECT_UNIFORM_HPP
#define OPTIMARU_SELECT_UNIFORM_HPP

#include <random>

#include "optimaru/select/scheme.hpp"
#include "optimaru/portable_random.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// RandomOperatorSelector
// ---------------------------------------------------------------------------
// Uniform selection, ignoring every outcome. The non-adaptive control: the
// baseline an adaptive scheme has to beat before its bookkeeping is worth it.
class RandomOperatorSelector final : public OperatorSelectionScheme {
public:
    explicit RandomOperatorSelector(int operator_count) : OperatorSelectionScheme(operator_count) {}

    int select(std::mt19937_64& rng) override { return random_index(rng, operator_count_); }
    void update(int, MoveOutcome) override {}
};

}  // namespace optimaru

#endif  // OPTIMARU_SELECT_UNIFORM_HPP

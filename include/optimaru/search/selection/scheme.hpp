// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_SELECTION_SCHEME_HPP
#define OPTIMARU_SEARCH_SELECTION_SCHEME_HPP

#include <cstdint>
#include <random>
#include <vector>

namespace optimaru {

// ---------------------------------------------------------------------------
// Adaptive operator selection for LNS (the "A" in ALNS)
// ---------------------------------------------------------------------------
// One abstract selection scheme with four implementations, all fed the same
// four-outcome reward vector:
//
//   select(rng)                -> which operator to apply this iteration
//   update(index, outcome)     -> observed outcome of that application
//   reset()                    -> forget everything (used after a perturbation)
//
// The outcome enum is shared with the engine's move/ruin bookkeeping.

enum class MoveOutcome { NewBest = 0, Better = 1, Accepted = 2, Rejected = 3 };

// Which adaptation strategy the ruin/move selection uses; one class per value
// in this directory.
enum class SelectionScheme {
    RouletteWheel,           // roulette_wheel.hpp
    SegmentedRouletteWheel,  // segmented_roulette_wheel.hpp
    AlphaUcb,                // alpha_ucb.hpp
    Uniform,                 // uniform.hpp
};

// ---------------------------------------------------------------------------
// OperatorSelectionScheme
// ---------------------------------------------------------------------------
class OperatorSelectionScheme {
public:
    explicit OperatorSelectionScheme(int operator_count) : operator_count_(operator_count) {}
    virtual ~OperatorSelectionScheme() = default;

    virtual int select(std::mt19937_64& rng) = 0;
    virtual void update(int operator_index, MoveOutcome outcome) = 0;
    virtual void reset() {}

protected:
    int operator_count_;
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_SELECTION_SCHEME_HPP

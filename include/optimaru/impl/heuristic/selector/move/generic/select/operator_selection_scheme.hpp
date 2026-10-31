#ifndef OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_SELECT_OPERATOR_SELECTION_SCHEME_HPP
#define OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_SELECT_OPERATOR_SELECTION_SCHEME_HPP

#include <cstdint>
#include <random>
#include <vector>

namespace optimaru {

// ---------------------------------------------------------------------------
// Adaptive operator selection for LNS (the "A" in ALNS)
// ---------------------------------------------------------------------------
// Defines one abstract selection scheme with
// four implementations, all fed the same four-outcome reward vector.
//
// Every scheme sees the same protocol as upstream's
// OperatorSelectionScheme:
//
//   select(rng)                -> which operator to apply this iteration
//   update(index, outcome)     -> observed outcome of that application
//   reset()                    -> forget everything (used after a perturbation)
//
// The outcome enum is shared with the engine's move/ruin bookkeeping.

enum class LnsOutcome { NEW_BEST = 0, BETTER = 1, ACCEPTED = 2, REJECTED = 3 };

// Which adaptation strategy the ruin/move selection uses. Mirrors the classes
// in this directory.
enum class OperatorSelectionType {
    ROULETTE_WHEEL,            // RouletteWheel
    SEGMENTED_ROULETTE_WHEEL,  // SegmentedRouletteWheel
    ALPHA_UCB,                 // AlphaUCB
    RANDOM,                    // RandomSelect
};

// ---------------------------------------------------------------------------
// OperatorSelectionScheme
// ---------------------------------------------------------------------------
class OperatorSelectionScheme {
public:
    explicit OperatorSelectionScheme(int operatorCount) : operatorCount_(operatorCount) {}
    virtual ~OperatorSelectionScheme() = default;

    virtual int select(std::mt19937_64& rng) = 0;
    virtual void update(int operatorIndex, LnsOutcome outcome) = 0;
    virtual void reset() {}

protected:
    int operatorCount_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_SELECT_OPERATOR_SELECTION_SCHEME_HPP

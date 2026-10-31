#ifndef BP_SCORE_CALCULATOR_HPP
#define BP_SCORE_CALCULATOR_HPP

#include "bp/domain.hpp"

namespace bp
{

// The bin-packing score, computed from scratch.
//
// calculateScore does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under FULL_ASSERT).
//
// Constraints (HardSoftScore), the default assembly of bp/constraints.hpp at
// the components' kDefaultWeight (every one is 1):
//   hard 1. binCapacityCost         -(load - capacity) per bin whose load
//                                   exceeds the capacity
//   hard 2. colorConflictCost       -(coloured items - distinct colours) per
//                                   bin: every item beyond the first of its
//                                   colour in a bin
//   hard 3. itemConflictCost        -1 per listed conflict pair whose two
//                                   items share a bin
//   soft 4. usedBinsCost            -1 per bin holding at least one item
// Unassigned items (bin == -1) contribute nothing.
class ScoreCalculator
{
public:
    static Score calculateScore(BinPackingProblem &problem);

    // Validates the facts and builds the derived lookups (colour classes,
    // resolved conflict pairs and partner lists) the calculators and the
    // components read. The lookups are rebuilt only while
    // BinPackingProblem::prepared is false; the checks on the planning
    // variable run on every call. Throws std::invalid_argument for:
    //   - a repeated item id ("item ids must be unique"),
    //   - an item weight <= 0 ("item weights must be positive"),
    //   - a conflict naming an unknown item or the same item twice,
    //   - a bin outside -1..itemCount()-1.
    // Must run before scoring; calculateScore and Solver::solve call it.
    static void prepare(BinPackingProblem &problem);

    // The checks on the planning variable alone (the last bullet above).
    static void requireBinsInRange(const BinPackingProblem &problem);
};

}  // namespace bp

#endif  // BP_SCORE_CALCULATOR_HPP

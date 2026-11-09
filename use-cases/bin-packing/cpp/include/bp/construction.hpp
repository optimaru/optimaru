#ifndef BP_CONSTRUCTION_HPP
#define BP_CONSTRUCTION_HPP

#include <random>
#include <vector>

#include "bp/domain.hpp"

namespace bp
{

// ---------------------------------------------------------------------------
// Domain construction (runs before the engine)
// ---------------------------------------------------------------------------
// The engine's construction heuristic would put each item in the FIRST bin
// of equal score (first fit); bin packing wants best fit. So the solver
// places every item itself, before the engine starts, and the engine's
// construction phase finds nothing left to do.

// Item indices by weight descending, then index (a stable order; the start
// order of the first worker).
std::vector<int> descendingOrder(const BinPackingProblem &problem);

// The same order with the items of equal weight shuffled among themselves:
// the start order of parallel worker i > 0.
std::vector<int> tieShuffledOrder(const BinPackingProblem &problem,
                                  std::mt19937_64 &rng);

// Best-fit decreasing around whatever the problem already holds: every item of `order` still at bin -1 goes, in that order,
// to the bin with the least room left after it among ALL bins
// 0..(highest bin in use) — empty gaps of a seed included —, ties to the
// lowest index. When it fits nowhere, a new bin is opened just past the
// highest one; once that would be bin n (only possible when a seed leaves
// gaps), the lowest-index empty bin is opened instead, so the result always
// stays inside the value range 0..n-1.
// Items already in a bin stay put.
void constructBins(BinPackingProblem &problem, const std::vector<int> &order);

// Relabels the bins by first appearance in item order:
// the bins in use become 0..k-1 without gaps. Score-neutral (every
// constraint is invariant under relabelling bins). Unassigned items stay -1.
void compactBins(BinPackingProblem &problem);

}  // namespace bp

#endif  // BP_CONSTRUCTION_HPP

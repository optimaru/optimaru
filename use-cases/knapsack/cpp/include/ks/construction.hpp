#ifndef KS_CONSTRUCTION_HPP
#define KS_CONSTRUCTION_HPP

#include <vector>

#include "ks/domain.hpp"

namespace ks
{

// The greedy construction (the Python reference's `_greedy_selection`):
// items in descending value / weight order — the density as a double, equal
// densities in item order (a stable sort) — are taken while the selected
// weight stays within the capacity. It looks at the weight capacity only.
//
// `fixed` holds one entry per item: 0 / 1 = decided (kept as given; the
// load starts at the weight of the items fixed to 1), -1 = undecided (the
// greedy decides it). Returns 0 / 1 per item. Requires
// ScoreCalculator::prepare(problem); throws std::invalid_argument when
// `fixed` has the wrong length or a value outside {-1, 0, 1}.
std::vector<int> greedySelection(const KnapsackProblem &problem,
                                 const std::vector<int> &fixed);

// The domain construction run by Solver::solve: every item from
// greedySelection(problem, all -1), or — when problem.warmStart — the current
// selection kept and only its undecided items (-1) filled in around the
// seeded load. Leaves every item at 0 or 1.
void constructSelection(KnapsackProblem &problem);

}  // namespace ks

#endif  // KS_CONSTRUCTION_HPP

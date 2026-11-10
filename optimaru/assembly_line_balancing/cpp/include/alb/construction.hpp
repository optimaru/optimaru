#ifndef ALB_CONSTRUCTION_HPP
#define ALB_CONSTRUCTION_HPP

#include <vector>

#include "alb/domain.hpp"

namespace alb {

// The domain-side construction the Solver runs before the engine — identical
// to the pure-Python reference's construction
// (tests/assembly_line_balancing/reference_assembly_line_balancing.py), so a zero-budget solve
// returns exactly the stations the reference returns.

// Level-synchronous Kahn order of the task indices: every task whose
// predecessors are all in earlier levels forms the next level, sorted by
// Task::id (std::string order is UTF-8 byte order, which equals Python's
// code-point order). A duplicate arc counts once per copy. Throws
// std::invalid_argument("precedence graph must be acyclic") when some tasks
// remain but none is ready (a cycle, a self-arc included).
std::vector<int> topologicalOrder(const AssemblyLineBalancingProblem& problem);

// First fit along `order`: tasks already assigned (station >= 0) are seeds and
// keep their station; the others go, in order, onto a station pointer that
// never moves down — it advances while the station is non-empty and the task
// would push it beyond the cycle time, and a seed pushes it up to its own
// station. Finally every unseeded task with a fixed_station is overwritten to
// it (without updating the loads). Writes Task::station; does not prepare.
void constructStations(AssemblyLineBalancingProblem& problem, const std::vector<int>& order);

}  // namespace alb

#endif  // ALB_CONSTRUCTION_HPP

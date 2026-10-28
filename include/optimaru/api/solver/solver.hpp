#ifndef OPTIMARU_API_SOLVER_SOLVER_HPP
#define OPTIMARU_API_SOLVER_SOLVER_HPP

#include "optimaru/impl/solver/default_solver.hpp"

namespace optimaru {

// Mirrors api.solver.Solver. Upstream's Solver is an interface with
// DefaultSolver as the implementation; header-only C++ has no reason for the
// virtual seam, so the public name is an alias.
template <typename Solution>
using Solver = DefaultSolver<Solution>;

}  // namespace optimaru

#endif  // OPTIMARU_API_SOLVER_SOLVER_HPP

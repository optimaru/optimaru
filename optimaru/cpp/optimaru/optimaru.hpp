// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_OPTIMARU_HPP
#define OPTIMARU_OPTIMARU_HPP

// Everything a use case's solver needs (python/ holds the pybind11 helpers
// a bindings.cpp includes itself). The directories:
//   model/      how the engine reads and writes a solution
//   score/      the score, its levels and the incremental objectives
//   construct/  the initial solution
//   accept/     acceptance rules          select/  adaptive operator selection
//   stop/       when a solve ends
//   search/         the step loop, and the plugin points for custom operators
//   moves/          every move the step loop can draw
//   ruin_recreate/  ruin operators + recreate: one destroy-and-repair unit

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/score/levels.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/solver_config.hpp"
#include "optimaru/search/operators.hpp"
#include "optimaru/worker_threads.hpp"

#endif  // OPTIMARU_OPTIMARU_HPP

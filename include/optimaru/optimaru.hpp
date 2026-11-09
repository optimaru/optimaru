// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_OPTIMARU_HPP
#define OPTIMARU_OPTIMARU_HPP

// Everything a use case needs:
//   solution_traits.hpp     how the engine reads and writes a solution
//   score.hpp, levels.hpp   the score and its levels
//   objective.hpp, ...      incremental objectives (decomposed, weighted, chain)
//   settings.hpp            what a solve is configured with
//   solver.hpp              running a solve (background_solve.hpp: on a thread)
//   explain.hpp             scoring and explaining a solution outside a solve
//   search/                 the search itself: construction, local search,
//                           acceptors, moves, ruins, strategies

#include "optimaru/solution_traits.hpp"
#include "optimaru/derived_state.hpp"
#include "optimaru/score.hpp"
#include "optimaru/chain_objective.hpp"
#include "optimaru/weighted_objective.hpp"
#include "optimaru/decomposed_objective.hpp"
#include "optimaru/objective.hpp"
#include "optimaru/score_breakdown.hpp"
#include "optimaru/editor.hpp"
#include "optimaru/explain.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/background_solve.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/search/operators.hpp"

#endif  // OPTIMARU_OPTIMARU_HPP

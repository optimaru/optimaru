#ifndef OPTIMARU_CONFIG_SOLVER_SOLVER_CONFIG_HPP
#define OPTIMARU_CONFIG_SOLVER_SOLVER_CONFIG_HPP

#include <cstdint>
#include <functional>

#include "optimaru/api/score/buildin/hard_soft_score.hpp"
#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/config/constructionheuristic/construction_heuristic_config.hpp"
#include "optimaru/config/localsearch/local_search_config.hpp"
#include "optimaru/config/solver/termination/termination_config.hpp"

namespace optimaru {

// Mirrors config.solver.SolverConfig (as plain C++ structs instead of XML):
// the top-level knobs plus one nested config per phase concern.
struct SolverConfig {
    std::uint64_t randomSeed = 0;

    // FULL_ASSERT re-verifies the incremental score against a full recompute
    // after every move (debugging aid; orders of magnitude slower).
    EnvironmentMode environmentMode = EnvironmentMode::NON_ASSERT;

    // Multi-start: solveMultiStart() runs this many independent solver
    // threads (seeds randomSeed, randomSeed+1, ...) and keeps the best.
    int multiStartCount = 1;

    std::function<void(std::int64_t, const HardSoftScore&)> onNewBest;

    TerminationConfig termination;
    ConstructionHeuristicConfig constructionHeuristic;
    LocalSearchConfig localSearch;
};

}  // namespace optimaru

#endif  // OPTIMARU_CONFIG_SOLVER_SOLVER_CONFIG_HPP

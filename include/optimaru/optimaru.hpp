#ifndef OPTIMARU_OPTIMARU_HPP
#define OPTIMARU_OPTIMARU_HPP

// Umbrella header: everything a quickstart needs. The layout under
// include/optimaru/ mirrors the upstream Java core's package structure
// (ai.timefold.solver.core.{api,config,impl,preview}).

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/domain/variable/variable_listener.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"
#include "optimaru/api/score/calculator/chained_incremental_score_calculator.hpp"
#include "optimaru/api/score/calculator/composite_incremental_score_calculator.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"
#include "optimaru/api/score/analysis/score_analysis.hpp"
#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/api/solver/solution_manager.hpp"
#include "optimaru/api/solver/solver.hpp"
#include "optimaru/api/solver/solver_factory.hpp"
#include "optimaru/api/solver/solver_manager.hpp"
#include "optimaru/config/solver/solver_config.hpp"
#include "optimaru/preview/api/move/operators.hpp"

#endif  // OPTIMARU_OPTIMARU_HPP

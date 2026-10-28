#ifndef OPTIMARU_API_SOLVER_SOLUTION_MANAGER_HPP
#define OPTIMARU_API_SOLVER_SOLUTION_MANAGER_HPP

#include "optimaru/api/score/analysis/score_analysis.hpp"
#include "optimaru/api/score/calculator/composite_incremental_score_calculator.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace optimaru {

// Mirrors api.solver.SolutionManager: out-of-band scoring and score
// explanation for a (solved or hand-built) solution. analyze() performs an
// authoritative full recompute (resetWorkingSolution) — it never trusts
// stale incremental state.
template <typename Solution>
class SolutionManager {
public:
    // Score explanation: per-constraint breakdown. Requires the constraints
    // to be registered as named components of a
    // CompositeIncrementalScoreCalculator.
    static ScoreAnalysis analyze(Solution& solution,
                                 CompositeIncrementalScoreCalculator<Solution>& calculator) {
        calculator.resetWorkingSolution(solution);
        return calculator.analyze();
    }

    // Monolithic calculator: total score only (a single unnamed entry).
    static ScoreAnalysis analyze(Solution& solution,
                                 IncrementalScoreCalculator<Solution>& calculator) {
        calculator.resetWorkingSolution(solution);
        ScoreAnalysis analysis;
        analysis.score = calculator.calculateScore();
        analysis.constraints.push_back({"score", analysis.score});
        return analysis;
    }

    static HardSoftScore update(Solution& solution,
                                IncrementalScoreCalculator<Solution>& calculator) {
        calculator.resetWorkingSolution(solution);
        return calculator.calculateScore();
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_API_SOLVER_SOLUTION_MANAGER_HPP

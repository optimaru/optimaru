#ifndef OPTIMARU_IMPL_SOLVER_TERMINATION_TERMINATION_CRITERION_HPP
#define OPTIMARU_IMPL_SOLVER_TERMINATION_TERMINATION_CRITERION_HPP

#include <chrono>
#include <cstdint>

#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// The termination plugin point (impl.solver.termination.Termination upstream).
// ---------------------------------------------------------------------------
// One criterion per file next to this one; SolverTermination OR-composes them.
// Adding a criterion is: a file here + one line in termination.hpp.

// Shared per-solve state the criteria read from. The composite owns it and
// refreshes it once per check, so a criterion never touches the clock itself.
struct TerminationContext {
    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point lastImprovement;
    std::chrono::steady_clock::time_point now;
    std::int64_t step = 0;
    std::int64_t lastImprovementStep = 0;
    HardSoftScore bestScore;
};

class Termination {
public:
    virtual ~Termination() = default;
    virtual bool isTerminated(const TerminationContext& context) const = 0;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_SOLVER_TERMINATION_TERMINATION_CRITERION_HPP

#ifndef OPTIMARU_CONFIG_SOLVER_TERMINATION_TERMINATION_CONFIG_HPP
#define OPTIMARU_CONFIG_SOLVER_TERMINATION_TERMINATION_CONFIG_HPP

#include <chrono>
#include <cstdint>
#include <limits>

#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace optimaru {

// Mirrors config.solver.termination.TerminationConfig: the solve stops as
// soon as ANY configured limit is hit (OR composition).
struct TerminationConfig {
    // Total wall time (always active).
    std::chrono::milliseconds spentLimit{std::chrono::seconds(30)};
    // Wall time since the last best-score improvement (0 = disabled).
    std::chrono::milliseconds unimprovedSpentLimit{0};
    // Local-search step count (0 = disabled).
    std::int64_t stepCountLimit = 0;
    // Steps since the last best-score improvement (0 = disabled).
    std::int64_t unimprovedStepCountLimit = 0;
    // Stop once the best score reaches this (default never fires).
    HardSoftScore bestScoreLimit{std::numeric_limits<std::int64_t>::max(),
                                 std::numeric_limits<double>::infinity()};
};

}  // namespace optimaru

#endif  // OPTIMARU_CONFIG_SOLVER_TERMINATION_TERMINATION_CONFIG_HPP

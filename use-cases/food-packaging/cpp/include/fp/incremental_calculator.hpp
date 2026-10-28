#ifndef FP_INCREMENTAL_CALCULATOR_HPP
#define FP_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <utility>
#include <vector>

#include "fp/domain.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace fp
{

// Incremental port of FoodPackagingConstraintProvider for the engine's LIST
// hook protocol (beforeListChanged / afterListChanged). Every constraint of
// this quickstart decomposes per line once the shadow chain is known:
//
//   - per line l: lateness sums (hard/medium), the makespan^2 term, the sum
//     of assigned job durations, and the list of non-empty cleaning windows
//     [startCleaning, startProduction) in position order,
//   - one cross-line term: cleaning-window overlap between line pairs that
//     share an operator (jobs on one line never overlap each other because
//     the windows are sequential),
//   - unassigned medium penalty = total job duration - assigned duration.
//
// A move touches at most a few lists; each hook retracts / re-derives ONE
// line in O(list length + same-operator overlap work) instead of rescoring
// the whole solution. The shadow chain is recomputed inside afterListChanged
// (list-position based, exactly ScoreCalculator::updateShadowVariables'
// semantics) rather than through a VariableListener, so the calculator stays
// self-contained and multi-start safe (the engine's solveMultiStart does not
// carry listeners into worker threads). The pool list (index lineCount) and
// engine-unassigned jobs contribute only the medium unassigned penalty.
class FoodPackagingIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<PackagingSchedule>
{
public:
    void resetWorkingSolution(PackagingSchedule &workingSolution) override;
    void beforeListChanged(int listIndex) override;
    void afterListChanged(int listIndex) override;
    optimaru::HardSoftScore calculateScore() override;

    // The maintained score as the HardMediumSoft triple (for tests).
    Score currentScore() const
    {
        return {-(totalHardLate_ + conflictTotal_),
                -(totalMediumLate_ + totalDuration_ - assignedDuration_),
                -totalMakespanSq_};
    }

private:
    struct LineState
    {
        std::int64_t hardLate = 0;    // sum max(0, end - maxEnd)
        std::int64_t mediumLate = 0;  // sum max(0, end - idealEnd)
        std::int64_t makespanSq = 0;  // (last end - line start)^2, 0 if empty
        std::int64_t assignedDuration = 0;
        // Non-empty cleaning windows in position order (starts and ends are
        // both non-decreasing along a line).
        std::vector<std::pair<Minutes, Minutes>> windows;
    };

    // Re-derives the shadow chain of line l and its per-line sums, in place:
    // the caller's windows buffer is cleared and refilled so its capacity is
    // reused across moves (afterListChanged runs millions of times per
    // second; a fresh vector per call showed up as allocator time).
    void deriveLine(int l, LineState &state) const;
    // Java's overlapMinutes summed over every overlapping window pair of two
    // lines (two-pointer over the sorted windows).
    static std::int64_t
    pairOverlap(const std::vector<std::pair<Minutes, Minutes>> &a,
                const std::vector<std::pair<Minutes, Minutes>> &b);

    PackagingSchedule *schedule_ = nullptr;
    std::vector<LineState> lineState_;
    std::vector<std::vector<int>> sameOperatorLines_;  // per line
    std::vector<std::int64_t> pairOverlap_;            // L x L, symmetric

    std::int64_t totalHardLate_ = 0;
    std::int64_t totalMediumLate_ = 0;
    std::int64_t totalMakespanSq_ = 0;
    std::int64_t conflictTotal_ = 0;
    std::int64_t totalDuration_ = 0;     // constant per working solution
    std::int64_t assignedDuration_ = 0;  // jobs currently on a real line
};

}  // namespace fp

#endif  // FP_INCREMENTAL_CALCULATOR_HPP

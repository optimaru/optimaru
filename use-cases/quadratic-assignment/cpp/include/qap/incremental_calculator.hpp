#ifndef QAP_INCREMENTAL_CALCULATOR_HPP
#define QAP_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"
#include "qap/domain.hpp"
#include "qap/score_calculator.hpp"

namespace qap
{

// The quadratic-assignment implementation of the engine's
// optimaru::IncrementalScoreCalculator: the monolithic fast path. A facility
// f moving is retracted at its old location and inserted at its new one:
//   - hard 1 (pinnedAssignmentsCost): re-check the k locations pinned to f,
//     O(k);
//   - hard 2 (forbiddenAssignmentsCost): one mask lookup, O(1);
//   - hard 3 (locationCapacityCost): re-clamp the one location's load, O(1);
//   - soft 4 (assignmentCost): one matrix lookup, O(1);
//   - soft 5 (flowDistanceCost): contrib(f, l) over the other assigned
//     facilities, O(n) — four times per swap (two retracts, two inserts).
//
// beforeVariableChanged retracts while `location` still holds the OLD value;
// afterVariableChanged inserts the NEW value. The calculator keeps its own
// location mirror, so the transient states inside a swap batch (two
// facilities on one location) stay exact. Unassigned facilities contribute
// nothing. The full-recompute reference is ScoreCalculator::calculateScore
// (checked in the tests and under EnvironmentMode::FULL_ASSERT).
class QuadraticAssignmentIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<QuadraticAssignmentProblem>
{
public:
    void resetWorkingSolution(QuadraticAssignmentProblem &workingSolution) override;
    void beforeVariableChanged(int facilityIndex) override;
    void afterVariableChanged(int facilityIndex) override;
    Score calculateScore() override;

    // The running raw totals, in ScoreCalculator::RawCosts form (the tests
    // compare them per constraint with a full recompute).
    ScoreCalculator::RawCosts rawCosts() const;

private:
    QuadraticAssignmentProblem *problem_ = nullptr;
    std::vector<int> location_;      // per facility (-1 == unassigned)
    std::vector<std::int64_t> load_;  // summed demand per location

    // Running raw totals (all counted positive; weights applied in
    // calculateScore).
    std::int64_t pinned_ = 0;     // hard 1
    std::int64_t forbidden_ = 0;  // hard 2
    std::int64_t overCapacity_ = 0;  // hard 3
    std::int64_t direct_ = 0;     // soft 4
    std::int64_t flow_ = 0;       // soft 5

    void insert(int facility, int location);
    void retract(int facility, int location);
    std::int64_t capacityPenalty(int location) const;
    std::int64_t flowContribution(int facility, int location) const;
};

}  // namespace qap

#endif  // QAP_INCREMENTAL_CALCULATOR_HPP

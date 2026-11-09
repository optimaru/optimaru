#ifndef QAP_INCREMENTAL_CALCULATOR_HPP
#define QAP_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "optimaru/objective.hpp"
#include "qap/domain.hpp"
#include "qap/score_calculator.hpp"

namespace qap
{

// The quadratic-assignment implementation of the engine's
// optimaru::Objective: the monolithic fast path. A facility
// f moving is retracted at its old location and inserted at its new one:
//   - hard 1 (pinnedAssignmentsCost): re-check the k locations pinned to f,
//     O(k);
//   - hard 2 (forbiddenAssignmentsCost): one mask lookup, O(1);
//   - hard 3 (locationCapacityCost): re-clamp the one location's load, O(1);
//   - soft 4 (assignmentCost): one matrix lookup, O(1);
//   - soft 5 (flowDistanceCost): contrib(f, l) over the other assigned
//     facilities, O(n) — four times per swap (two retracts, two inserts).
//
// before_assign retracts while `location` still holds the OLD value;
// after_assign inserts the NEW value. The calculator keeps its own
// location mirror, so the transient states inside a swap batch (two
// facilities on one location) stay exact. Unassigned facilities contribute
// nothing. The full-recompute reference is ScoreCalculator::score
// (checked in the tests and under ScoreChecks::ScoreChecks::Full).
class QuadraticAssignmentIncrementalScoreCalculator final
    : public optimaru::Objective<QuadraticAssignmentProblem>
{
public:
    void rebuild(QuadraticAssignmentProblem &solution) override;
    void before_assign(int facilityIndex) override;
    void after_assign(int facilityIndex) override;
    Score score() override;

    // The running raw totals, in ScoreCalculator::RawCosts form (the tests
    // compare them per constraint with a full recompute).
    ScoreCalculator::RawCosts rawCosts() const;

private:
    QuadraticAssignmentProblem *problem_ = nullptr;
    std::vector<int> location_;      // per facility (-1 == unassigned)
    std::vector<std::int64_t> load_;  // summed demand per location

    // Running raw totals (all counted positive; weights applied in
    // score).
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

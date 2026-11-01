#ifndef FAB_INCREMENTAL_CALCULATOR_HPP
#define FAB_INCREMENTAL_CALCULATOR_HPP

#include "fab/domain.hpp"
#include "fab/evaluator.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace fab
{

// ---------------------------------------------------------------------------
// SemiconductorIncrementalScoreCalculator — the default fast path
// ---------------------------------------------------------------------------
// The ScheduleEvaluator at the default weights: a planning-value change only
// records the row; calculateScore() re-decodes the suffix that can change
// (from the earliest readyStep of the changed rows) and re-costs only the
// units whose placements moved. A swap's two writes therefore cost one
// suffix decode. Equal to ScoreCalculator::searchStateScore after every move
// (FULL_ASSERT and fab_test check it).
class SemiconductorIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<SemiconductorSchedule>
{
public:
    void resetWorkingSolution(SemiconductorSchedule &schedule) override;
    void beforeVariableChanged(int row) override;
    void afterVariableChanged(int row) override;
    optimaru::HardSoftScore calculateScore() override;

    // The raw totals behind calculateScore() (tests compare them term by
    // term with the full recompute).
    const RawTotals &rawTotals() { return evaluator_.totals(); }
    const ScheduleEvaluator &evaluator() const { return evaluator_; }

private:
    ScheduleEvaluator evaluator_;
};

}  // namespace fab

#endif  // FAB_INCREMENTAL_CALCULATOR_HPP

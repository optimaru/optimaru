#ifndef FAB_INCREMENTAL_CALCULATOR_HPP
#define FAB_INCREMENTAL_CALCULATOR_HPP

#include "fab/domain.hpp"
#include "fab/evaluator.hpp"
#include "optimaru/score/objective.hpp"

namespace fab {

// ---------------------------------------------------------------------------
// SemiconductorIncrementalScoreCalculator — the default fast path
// ---------------------------------------------------------------------------
// The ScheduleEvaluator at the default weights: a decision-value change only
// records the row; score() re-decodes the suffix that can change
// (from the earliest readyStep of the changed rows) and re-costs only the
// units whose placements moved. A swap's two writes therefore cost one
// suffix decode. Equal to ScoreCalculator::searchStateScore after every move
// (ScoreChecks::Full and fab_test check it).
class SemiconductorIncrementalScoreCalculator final
    : public optimaru::Objective<SemiconductorSchedule> {
public:
    void rebuild(SemiconductorSchedule& schedule) override;
    void before_assign(int row) override;
    void after_assign(int row) override;
    optimaru::Score score() override;

    // The raw totals behind score() (tests compare them term by
    // term with the full recompute).
    const RawTotals& rawTotals() { return evaluator_.totals(); }
    const ScheduleEvaluator& evaluator() const { return evaluator_; }

private:
    ScheduleEvaluator evaluator_;
};

}  // namespace fab

#endif  // FAB_INCREMENTAL_CALCULATOR_HPP

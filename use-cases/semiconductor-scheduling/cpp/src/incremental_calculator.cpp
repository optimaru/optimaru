#include "fab/incremental_calculator.hpp"

#include "fab/score_calculator.hpp"

namespace fab
{

void SemiconductorIncrementalScoreCalculator::resetWorkingSolution(SemiconductorSchedule &schedule)
{
    ScoreCalculator::prepare(schedule);
    evaluator_.reset(schedule);
}

void SemiconductorIncrementalScoreCalculator::beforeVariableChanged(int /*row*/) {}

void SemiconductorIncrementalScoreCalculator::afterVariableChanged(int row)
{
    evaluator_.markChanged(row);
}

optimaru::HardSoftScore SemiconductorIncrementalScoreCalculator::calculateScore()
{
    return ScoreCalculator::scoreOf(evaluator_.totals());
}

}  // namespace fab

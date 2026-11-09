#include "fab/incremental_calculator.hpp"

#include "fab/score_calculator.hpp"

namespace fab
{

void SemiconductorIncrementalScoreCalculator::rebuild(SemiconductorSchedule &schedule)
{
    ScoreCalculator::prepare(schedule);
    evaluator_.reset(schedule);
}

void SemiconductorIncrementalScoreCalculator::before_assign(int /*row*/) {}

void SemiconductorIncrementalScoreCalculator::after_assign(int row)
{
    evaluator_.markChanged(row);
}

optimaru::Score SemiconductorIncrementalScoreCalculator::score()
{
    return ScoreCalculator::scoreOf(evaluator_.totals());
}

}  // namespace fab

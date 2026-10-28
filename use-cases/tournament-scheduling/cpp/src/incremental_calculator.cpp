#include "ts/incremental_calculator.hpp"

#include <cstddef>

#include "ts/score_calculator.hpp"

namespace ts
{

void TournamentSchedulingIncrementalScoreCalculator::resetWorkingSolution(
    TournamentSchedule &workingSolution)
{
    schedule_ = &workingSolution;
    teamCount_ = workingSolution.teamCount();
    dayCount_ = workingSolution.dayCount();

    const std::size_t cells = static_cast<std::size_t>(teamCount_) * dayCount_;
    cellCnt_.assign(cells, 0);
    penaltyCnt_.assign(cells, 0);
    teamLoad_.assign(teamCount_, 0);
    confCnt_.assign(static_cast<std::size_t>(teamCount_) * teamCount_, 0);
    assign_.assign(workingSolution.teamAssignments.size(), -1);

    pairSum_ = 0;
    unavailHit_ = 0;
    balSum_ = 0;
    balSumSq_ = 0;
    balN_ = 0;
    confSum_ = 0;
    confSumSq_ = 0;
    confN_ = 0;

    for (const UnavailabilityPenalty &penalty :
         workingSolution.unavailabilityPenalties)
    {
        ++penaltyCnt_[cellOffset(penalty.team, penalty.day)];
    }

    const int assignmentCount
        = static_cast<int>(workingSolution.teamAssignments.size());
    for (int i = 0; i < assignmentCount; ++i)
    {
        const int team = workingSolution.teamAssignments[i].team;
        if (team >= 0)
            insert(i, team);
    }
}

void TournamentSchedulingIncrementalScoreCalculator::addConfrontations(
    int t1, int t2, std::int64_t delta)
{
    if (t1 > t2)
    {
        const int tmp = t1;
        t1 = t2;
        t2 = tmp;
    }
    std::int64_t &count = confCnt_[pairOffset(t1, t2)];
    if (count == 0 && delta > 0)
        ++confN_;
    confSum_ += delta;
    confSumSq_ += (count + delta) * (count + delta) - count * count;
    count += delta;
    if (count == 0 && delta < 0)
        --confN_;
}

void TournamentSchedulingIncrementalScoreCalculator::insert(int assignmentIndex,
                                                            int team)
{
    const int day = schedule_->teamAssignments[assignmentIndex].day;
    const std::size_t cell = cellOffset(team, day);

    // hard 1: joining a cell with c members adds c same-team same-day pairs.
    const int c = cellCnt_[cell];
    pairSum_ += c;
    // hard 2: the cell's occupancy crosses 0 -> 1: its penalty facts hit.
    if (c == 0)
        unavailHit_ += penaltyCnt_[cell];

    // soft 4: one new confrontation with every other team's assignment on
    // this day (same-team pairs are excluded by the Java lessThan(teamId)).
    for (int u = 0; u < teamCount_; ++u)
    {
        if (u == team)
            continue;
        const int k = cellCnt_[cellOffset(u, day)];
        if (k > 0)
            addConfrontations(team, u, k);
    }
    ++cellCnt_[cell];

    // medium 3: the team's load grows by 1.
    const std::int64_t load = teamLoad_[team];
    if (load == 0)
        ++balN_;
    balSum_ += 1;
    balSumSq_ += 2 * load + 1;
    ++teamLoad_[team];

    assign_[assignmentIndex] = team;
}

void TournamentSchedulingIncrementalScoreCalculator::retract(
    int assignmentIndex, int team)
{
    const int day = schedule_->teamAssignments[assignmentIndex].day;
    const std::size_t cell = cellOffset(team, day);

    --cellCnt_[cell];
    const int c = cellCnt_[cell];
    pairSum_ -= c;
    if (c == 0)
        unavailHit_ -= penaltyCnt_[cell];

    for (int u = 0; u < teamCount_; ++u)
    {
        if (u == team)
            continue;
        const int k = cellCnt_[cellOffset(u, day)];
        if (k > 0)
            addConfrontations(team, u, -k);
    }

    const std::int64_t load = teamLoad_[team];
    balSum_ -= 1;
    balSumSq_ += (load - 1) * (load - 1) - load * load;
    --teamLoad_[team];
    if (teamLoad_[team] == 0)
        --balN_;

    assign_[assignmentIndex] = -1;
}

void TournamentSchedulingIncrementalScoreCalculator::beforeVariableChanged(
    int assignmentIndex)
{
    const int team = assign_[assignmentIndex];
    if (team >= 0)
        retract(assignmentIndex, team);
}

void TournamentSchedulingIncrementalScoreCalculator::afterVariableChanged(
    int assignmentIndex)
{
    const int team = schedule_->teamAssignments[assignmentIndex].team;
    if (team >= 0)
        insert(assignmentIndex, team);
}

Score TournamentSchedulingIncrementalScoreCalculator::calculateScore()
{
    Score score;
    score.hard = -(pairSum_ + unavailHit_);
    score.soft = ScoreCalculator::combinedSoft(
        ScoreCalculator::unfairnessFromAggregates(balSum_, balSumSq_, balN_),
        ScoreCalculator::unfairnessFromAggregates(
            confSum_, confSumSq_, confN_));
    return score;
}

}  // namespace ts

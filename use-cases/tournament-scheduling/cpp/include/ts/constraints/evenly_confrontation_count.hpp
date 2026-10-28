#ifndef TS_CONSTRAINTS_EVENLY_CONFRONTATION_COUNT_HPP
#define TS_CONSTRAINTS_EVENLY_CONFRONTATION_COUNT_HPP

#include "ts/constraints/common.hpp"

namespace ts
{

// Soft (default, weight 1): evenlyConfrontationCount — the LoadBalance
// unfairness of the confrontation counts per team pair. Two teams confront
// each other once per pair of same-day assignments they hold (the Java
// lessThan(teamId) join, so a team never confronts itself), and only pairs
// with at least one confrontation enter the balance.
//
// Global because the statistic is over team PAIRS, which no single unit owns.
// It keeps the same state the monolithic calculator does — per-(team, day)
// occupancy plus the per-pair counts and the three unfairness aggregates —
// so one join/leave costs O(teamCount) and is exactly reversible.
struct EvenlyConfrontationCount final : MemberChangeGlobalConstraint
{
    explicit EvenlyConfrontationCount(double weight = 1.0)
        : MemberChangeGlobalConstraint("evenlyConfrontationCount",
                                       weight,
                                       /*hard=*/false)
    {
    }

    void reset(const TournamentSchedule &schedule,
               const std::vector<std::vector<int>> &membersPerUnit) override
    {
        teamCount_ = schedule.teamCount();
        dayCount_ = schedule.dayCount();
        cells_.assign(static_cast<std::size_t>(teamCount_) * dayCount_, 0);
        pairs_.assign(static_cast<std::size_t>(teamCount_) * teamCount_, 0);
        sum_ = 0;
        sumSquares_ = 0;
        loaded_ = 0;
        for (std::size_t unit = 0; unit < membersPerUnit.size(); ++unit)
        {
            for (int assignment : membersPerUnit[unit])
                apply(schedule, static_cast<int>(unit), assignment, true);
        }
    }

    void onMemberChanged(const TournamentSchedule &schedule,
                         int unit,
                         const std::vector<int> & /*members*/,
                         int entity,
                         bool joined) override
    {
        apply(schedule, unit, entity, joined);
    }

    double cost() const override
    {
        return ScoreCalculator::unfairnessFromAggregates(
            sum_, sumSquares_, loaded_);
    }

private:
    // One assignment of `team` joins (or leaves) day `day`. On a join the
    // confrontations are added against the OTHER teams' current occupancy and
    // only then is the cell incremented; on a leave the cell is decremented
    // first — the mirror image, which keeps the aggregates exactly reversible.
    void apply(const TournamentSchedule &schedule,
               int team,
               int assignmentIndex,
               bool joined)
    {
        const int day = detail::dayOf(schedule, assignmentIndex);
        const std::size_t cell = cellOffset(team, day);
        if (!joined)
            --cells_[cell];
        for (int other = 0; other < teamCount_; ++other)
        {
            if (other == team)
                continue;
            const std::int64_t occupancy = cells_[cellOffset(other, day)];
            if (occupancy > 0)
                addConfrontations(team, other, joined ? occupancy : -occupancy);
        }
        if (joined)
            ++cells_[cell];
    }

    void addConfrontations(int first, int second, std::int64_t delta)
    {
        if (first > second)
        {
            const int swap = first;
            first = second;
            second = swap;
        }
        std::int64_t &count
            = pairs_[static_cast<std::size_t>(first) * teamCount_ + second];
        if (count == 0 && delta > 0)
            ++loaded_;
        sum_ += delta;
        sumSquares_ += (count + delta) * (count + delta) - count * count;
        count += delta;
        if (count == 0 && delta < 0)
            --loaded_;
    }

    std::size_t cellOffset(int team, int day) const
    {
        return static_cast<std::size_t>(team) * dayCount_ + day;
    }

    int teamCount_ = 0;
    int dayCount_ = 0;
    std::vector<std::int64_t> cells_;  // (team, day) -> assignment count
    std::vector<std::int64_t> pairs_;  // (t1, t2), t1 < t2 -> confrontations
    std::int64_t sum_ = 0;
    std::int64_t sumSquares_ = 0;
    int loaded_ = 0;  // team pairs with >= 1 confrontation
};

}  // namespace ts

#endif  // TS_CONSTRAINTS_EVENLY_CONFRONTATION_COUNT_HPP

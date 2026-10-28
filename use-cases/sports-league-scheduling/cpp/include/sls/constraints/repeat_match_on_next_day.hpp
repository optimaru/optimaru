#ifndef SLS_CONSTRAINTS_REPEAT_MATCH_ON_NEXT_DAY_HPP
#define SLS_CONSTRAINTS_REPEAT_MATCH_ON_NEXT_DAY_HPP

#include "sls/constraints/common.hpp"

namespace sls
{

// Hard (default, weight 1): repeatMatchOnNextDay — a match whose REVERSED
// fixture is played on the very next round index. At most one point per match
// however many reversed fixtures sit there (the Java ifExists), which is what
// keeps this from being a pair sum and gives it its own state instead of the
// RoundIndexPairConstraint base.
//
// Two counters carry it: how many matches of each ordered team pair sit on
// each round index, and how many matches whose REVERSE is a given pair sit
// there. The total is then the number of assigned matches m for which the
// pair count of reverse(m) on index(m) + 1 is non-zero, maintained through
// two events per move:
//   - the moved match may itself gain or lose its own indicator;
//   - its pair count on the index it enters or leaves may cross 0 <-> 1,
//     which flips the indicator of every match on the PREVIOUS index whose
//     reverse is that pair.
// Both are evaluated against the pre-state on a join and the post-state on a
// leave, so the aggregate is exactly reversible.
struct RepeatMatchOnNextDay final : SlsGlobalConstraint
{
    explicit RepeatMatchOnNextDay(double weight = 1.0)
        : SlsGlobalConstraint("repeatMatchOnNextDay", weight, /*hard=*/true)
    {
    }

    bool supportsMemberChange() const override { return true; }
    void onUnitChanged(const LeagueSchedule & /*schedule*/,
                       int /*unit*/,
                       const std::vector<int> & /*members*/) override
    {
        throw std::logic_error(name
                               + ": onUnitChanged is unreachable "
                                 "(member-change fast path)");
    }

    void reset(const LeagueSchedule &schedule,
               const std::vector<std::vector<int>> &membersPerUnit) override
    {
        detail::requirePrepared(schedule);
        indexCount_ = schedule.maxRoundIndex + 1;
        const std::size_t cells
            = static_cast<std::size_t>(schedule.pairCount) * indexCount_;
        pairAt_.assign(cells, 0);
        reverseAt_.assign(cells, 0);
        total_ = 0;
        for (std::size_t unit = 0; unit < membersPerUnit.size(); ++unit)
        {
            const int index = schedule.roundIndexOf(static_cast<int>(unit));
            for (int matchIndex : membersPerUnit[unit])
                apply(schedule, matchIndex, index, /*joined=*/true);
        }
    }

    void onMemberChanged(const LeagueSchedule &schedule,
                         int unit,
                         const std::vector<int> & /*members*/,
                         int entity,
                         bool joined) override
    {
        apply(schedule, entity, schedule.roundIndexOf(unit), joined);
    }

    double cost() const override { return static_cast<double>(total_); }

private:
    void apply(const LeagueSchedule &schedule,
               int matchIndex,
               int index,
               bool joined)
    {
        const Match &match = schedule.matches[matchIndex];
        const int pair = match.pairId;
        const int reverse = match.reversePairId;
        if (joined)
        {
            total_ += ownIndicator(reverse, index);
            total_ += crossings(pair, index);
            if (pair >= 0)
                ++pairAt_[cell(pair, index)];
            if (reverse >= 0)
                ++reverseAt_[cell(reverse, index)];
            return;
        }
        if (pair >= 0)
            --pairAt_[cell(pair, index)];
        if (reverse >= 0)
            --reverseAt_[cell(reverse, index)];
        total_ -= ownIndicator(reverse, index);
        total_ -= crossings(pair, index);
    }

    // 1 when the reversed fixture is present on the next round index.
    std::int64_t ownIndicator(int reversePair, int index) const
    {
        if (reversePair < 0 || index + 1 >= indexCount_)
            return 0;
        return pairAt_[cell(reversePair, index + 1)] > 0 ? 1 : 0;
    }

    // How many matches on the PREVIOUS index flip their indicator because
    // this match's pair count on `index` crosses 0 <-> 1.
    std::int64_t crossings(int pair, int index) const
    {
        if (pair < 0 || index - 1 < 0)
            return 0;
        // On a join the counter has not been incremented yet, on a leave it
        // has already been decremented, so "currently zero" is the crossing
        // condition in both directions.
        if (pairAt_[cell(pair, index)] != 0)
            return 0;
        return reverseAt_[cell(pair, index - 1)];
    }

    std::size_t cell(int pair, int index) const
    {
        return static_cast<std::size_t>(pair) * indexCount_ + index;
    }

    int indexCount_ = 0;
    std::vector<std::int64_t> pairAt_;     // (pair, index) -> matches there
    std::vector<std::int64_t> reverseAt_;  // (pair, index) -> matches whose
                                           // REVERSE is that pair
    std::int64_t total_ = 0;
};

}  // namespace sls

#endif  // SLS_CONSTRAINTS_REPEAT_MATCH_ON_NEXT_DAY_HPP

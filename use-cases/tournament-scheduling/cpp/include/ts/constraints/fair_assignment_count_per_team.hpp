#ifndef TS_CONSTRAINTS_FAIR_ASSIGNMENT_COUNT_PER_TEAM_HPP
#define TS_CONSTRAINTS_FAIR_ASSIGNMENT_COUNT_PER_TEAM_HPP

#include "ts/constraints/common.hpp"

namespace ts
{

// Medium (default, weight kMediumScale): fairAssignmentCountPerTeam — the
// LoadBalance unfairness of the per-team assignment counts. Only teams that
// actually hold an assignment enter the balance (the Java rule has no
// complement), which is why `n` is maintained separately from the team count.
//
// Global because an unfairness is a statistic across units, not a sum over
// them. Its three aggregates (sum, sum of squares, loaded-team count) update
// in O(1) per join/leave and are exactly reversible, so it rides the
// member-change fast path.
struct FairAssignmentCountPerTeam final : MemberChangeGlobalConstraint
{
    explicit FairAssignmentCountPerTeam(double weight = kMediumScale)
        : MemberChangeGlobalConstraint("fairAssignmentCountPerTeam",
                                       weight,
                                       /*hard=*/false)
    {
    }
    void reset(const TournamentSchedule & /*schedule*/,
               const std::vector<std::vector<int>> &membersPerUnit) override
    {
        loads_.assign(membersPerUnit.size(), 0);
        sum_ = 0;
        sumSquares_ = 0;
        loaded_ = 0;
        for (std::size_t unit = 0; unit < membersPerUnit.size(); ++unit)
        {
            const std::int64_t load
                = static_cast<std::int64_t>(membersPerUnit[unit].size());
            loads_[unit] = load;
            if (load == 0)
                continue;
            sum_ += load;
            sumSquares_ += load * load;
            ++loaded_;
        }
    }
    void onMemberChanged(const TournamentSchedule & /*schedule*/,
                         int unit,
                         const std::vector<int> & /*members*/,
                         int /*entity*/,
                         bool joined) override
    {
        std::int64_t &load = loads_[unit];
        if (joined)
        {
            if (load == 0)
                ++loaded_;
            sum_ += 1;
            sumSquares_ += 2 * load + 1;
            ++load;
            return;
        }
        sum_ -= 1;
        sumSquares_ += (load - 1) * (load - 1) - load * load;
        --load;
        if (load == 0)
            --loaded_;
    }
    double cost() const override
    {
        return ScoreCalculator::unfairnessFromAggregates(
            sum_, sumSquares_, loaded_);
    }

private:
    std::vector<std::int64_t> loads_;  // per unit: assignment count
    std::int64_t sum_ = 0;
    std::int64_t sumSquares_ = 0;
    int loaded_ = 0;  // teams with load >= 1 (the unfairness item count)
};

}  // namespace ts

#endif  // TS_CONSTRAINTS_FAIR_ASSIGNMENT_COUNT_PER_TEAM_HPP

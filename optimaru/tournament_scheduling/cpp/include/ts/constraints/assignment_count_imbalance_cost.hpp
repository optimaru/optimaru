#ifndef TS_CONSTRAINTS_ASSIGNMENT_COUNT_IMBALANCE_COST_HPP
#define TS_CONSTRAINTS_ASSIGNMENT_COUNT_IMBALANCE_COST_HPP

#include "ts/constraints/common.hpp"

namespace ts
{

// Medium, weight 1 (medium units): assignmentCountImbalanceCost — how unevenly
// the assignments are spread over the teams. Raw cost (global) = the
// LoadBalance unfairness of the per-team assignment counts,
// sqrt(sum(load^2) - sum(load)^2 / n) rounded to 6 significant figures, so
// 0 = every loaded team holds the same number of assignments. Only teams that
// actually hold an assignment enter the balance (an unloaded team is not
// counted as load 0), which is why `n` is maintained separately from the team count.
//
// Folded into the engine's soft level as `medium * kMediumScale` (1e6) by the
// calculator (kMediumFold in domain.hpp), not by this weight.
//
// Global because an unfairness is a statistic across units, not a sum over
// them. Its three aggregates (sum, sum of squares, loaded-team count) update
// in O(1) per join/leave and are exactly reversible, so it rides the
// member-change fast path.
struct AssignmentCountImbalanceCost final : MemberChangeGlobalConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Medium;

    explicit AssignmentCountImbalanceCost(double weight = kDefaultWeight)
        : MemberChangeGlobalConstraint("assignmentCountImbalanceCost",
                                       weight,
                                       kDefaultLevel)
    {
    }
    void reset(const TournamentSchedule & /*schedule*/,
               const std::vector<std::vector<int>> &members_per_unit) override
    {
        loads_.assign(members_per_unit.size(), 0);
        sum_ = 0;
        sumSquares_ = 0;
        loaded_ = 0;
        for (std::size_t unit = 0; unit < members_per_unit.size(); ++unit)
        {
            const std::int64_t load
                = static_cast<std::int64_t>(members_per_unit[unit].size());
            loads_[unit] = load;
            if (load == 0)
                continue;
            sum_ += load;
            sumSquares_ += load * load;
            ++loaded_;
        }
    }
    void on_member_changed(const TournamentSchedule & /*schedule*/,
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

#endif  // TS_CONSTRAINTS_ASSIGNMENT_COUNT_IMBALANCE_COST_HPP

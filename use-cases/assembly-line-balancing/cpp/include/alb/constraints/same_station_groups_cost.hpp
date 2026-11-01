#ifndef ALB_CONSTRAINTS_SAME_STATION_GROUPS_COST_HPP
#define ALB_CONSTRAINTS_SAME_STATION_GROUPS_COST_HPP

#include <cstdint>
#include <utility>
#include <vector>

#include "alb/constraints/common.hpp"

namespace alb
{

// Hard, weight 1: sameStationGroupsCost — same-station groups split over
// several stations. A group costs max(0, distinct stations of its assigned
// members - 1) (a task listed twice counts once); raw cost = the sum over the
// groups, so 0 = every group on one station. One total (a global), so the
// exponent shapes that sum (the 0.1.5 pure-Python package shaped each
// group's term; the two agree at exponent 1).
//
// Member-change fast path: per group a small station -> member-count tally
// and its distinct-station count; a join or leave updates every group of the
// task, O(groups of the task * group size).
struct SameStationGroupsCost final : MemberChangeGlobalConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit SameStationGroupsCost(double weight = kDefaultWeight)
        : MemberChangeGlobalConstraint("sameStationGroupsCost", weight, kDefaultLevel)
    {
    }

    void reset(const AssemblyLineBalancingProblem &problem,
               const std::vector<std::vector<int>> &membersPerUnit) override
    {
        detail::requirePrepared(problem);
        tally_.assign(problem.groupMembers.size(), {});
        distinct_.assign(problem.groupMembers.size(), 0);
        extra_ = 0;
        for (std::size_t unit = 0; unit < membersPerUnit.size(); ++unit)
        {
            for (int task : membersPerUnit[unit])
                apply(problem, static_cast<int>(unit), task, true);
        }
    }

    void onMemberChanged(const AssemblyLineBalancingProblem &problem,
                         int unit,
                         const std::vector<int> & /*members*/,
                         int entity,
                         bool joined) override
    {
        apply(problem, unit, entity, joined);
    }

    double cost() const override { return static_cast<double>(extra_); }

private:
    static std::int64_t excess(int distinct) { return distinct > 1 ? distinct - 1 : 0; }

    void apply(const AssemblyLineBalancingProblem &problem, int station, int task, bool joined)
    {
        for (int group : problem.groupsOf[task])
        {
            std::vector<std::pair<int, int>> &tally = tally_[group];
            std::size_t at = 0;
            while (at < tally.size() && tally[at].first != station)
                ++at;
            const int before = distinct_[group];
            if (joined)
            {
                if (at == tally.size())
                {
                    tally.emplace_back(station, 1);
                    ++distinct_[group];
                }
                else
                {
                    ++tally[at].second;
                }
            }
            else
            {
                if (--tally[at].second == 0)
                {
                    tally[at] = tally.back();
                    tally.pop_back();
                    --distinct_[group];
                }
            }
            extra_ += excess(distinct_[group]) - excess(before);
        }
    }

    // Per group: (station, assigned members on it) for the stations in use.
    std::vector<std::vector<std::pair<int, int>>> tally_;
    std::vector<int> distinct_;  // per group: stations in use
    std::int64_t extra_ = 0;     // sum of max(0, distinct - 1)
};

}  // namespace alb

#endif  // ALB_CONSTRAINTS_SAME_STATION_GROUPS_COST_HPP

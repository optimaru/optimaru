#ifndef ALB_CONSTRAINTS_USED_STATIONS_COST_HPP
#define ALB_CONSTRAINTS_USED_STATIONS_COST_HPP

#include <cstdint>
#include <vector>

#include "alb/constraints/common.hpp"

namespace alb
{

// Soft, weight 1: usedStationsCost — the stations the line opens. Raw cost =
// the number of stations with a load > 0 (durations are > 0, so: stations
// with at least one task, CP-SAT's used[s]); 0 = no task is assigned.
//
// A global on purpose, not a per-station 0/1 unit: the exponent then shapes
// the station count itself.
// Member-change fast path: a station's first task opens it, its last one
// closes it, O(1). A step function no single-task move can cross, which is
// why the solver ships CloseStationMove.
struct UsedStationsCost final : MemberChangeGlobalConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit UsedStationsCost(double weight = kDefaultWeight)
        : MemberChangeGlobalConstraint("usedStationsCost", weight, kDefaultLevel)
    {
    }

    void reset(const AssemblyLineBalancingProblem &problem,
               const std::vector<std::vector<int>> &members_per_unit) override
    {
        detail::requirePrepared(problem);
        used_ = 0;
        for (const std::vector<int> &members : members_per_unit)
            used_ += members.empty() ? 0 : 1;
    }

    void on_member_changed(const AssemblyLineBalancingProblem & /*problem*/,
                         int /*unit*/,
                         const std::vector<int> &members,
                         int /*entity*/,
                         bool joined) override
    {
        // `members` is the station AFTER the change.
        if (joined && members.size() == 1)
            ++used_;
        else if (!joined && members.empty())
            --used_;
    }

    double cost() const override { return static_cast<double>(used_); }

private:
    std::int64_t used_ = 0;
};

}  // namespace alb

#endif  // ALB_CONSTRAINTS_USED_STATIONS_COST_HPP

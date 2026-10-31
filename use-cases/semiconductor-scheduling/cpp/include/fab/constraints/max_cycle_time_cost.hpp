#ifndef FAB_CONSTRAINTS_MAX_CYCLE_TIME_COST_HPP
#define FAB_CONSTRAINTS_MAX_CYCLE_TIME_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab
{

// Hard, weight 1: maxCycleTimeCost — lot cycle time beyond its maximum.
// Raw cost per lot with max_cycle_time set and its last route step assigned =
// max(0, last end - release time - max_cycle_time). 0 = every lot completes
// within its maximum cycle time.
struct MaxCycleTimeCost final : ScheduleViewConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;
    static constexpr Term kTerm = Term::MaxCycleTimeCost;

    explicit MaxCycleTimeCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("maxCycleTimeCost", weight, kDefaultLevel, kTerm)
    {
    }

    static std::int64_t lotCost(const SemiconductorSchedule &schedule, int lot,
                                const ScorePlacement *const *route,
                                std::vector<std::int64_t> *items)
    {
        const Lot &l = schedule.lots[static_cast<std::size_t>(lot)];
        const ScorePlacement *last = route[l.operations.size() - 1];
        if (!l.maxCycleTime || last == nullptr)
            return 0;
        return terms::emit(items, std::max<Time>(0, last->end - l.releaseTime - *l.maxCycleTime));
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_MAX_CYCLE_TIME_COST_HPP

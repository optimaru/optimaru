#ifndef FAB_CONSTRAINTS_CYCLE_TIME_COST_HPP
#define FAB_CONSTRAINTS_CYCLE_TIME_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab
{

// Soft, weight 1: cycleTimeCost — elapsed time from lot release to
// completion. Raw cost per lot with its last route step assigned =
// max(0, last end - release time); the sum is also the schedule's
// total_cycle_time. 0 = no lot with a completed route takes any time (a
// lower bound of the soft score, never reached by real work).
struct CycleTimeCost final : ScheduleViewConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;
    static constexpr Term kTerm = Term::CycleTimeCost;

    explicit CycleTimeCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("cycleTimeCost", weight, kDefaultLevel, kTerm)
    {
    }

    static std::int64_t lotCost(const SemiconductorSchedule &schedule, int lot,
                                const ScorePlacement *const *route,
                                std::vector<std::int64_t> *items)
    {
        const Lot &l = schedule.lots[static_cast<std::size_t>(lot)];
        const ScorePlacement *last = route[l.operations.size() - 1];
        if (last == nullptr)
            return 0;
        return terms::emit(items, std::max<Time>(0, last->end - l.releaseTime));
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_CYCLE_TIME_COST_HPP

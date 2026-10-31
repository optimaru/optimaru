#ifndef FAB_CONSTRAINTS_MIN_QUEUE_TIME_COST_HPP
#define FAB_CONSTRAINTS_MIN_QUEUE_TIME_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab
{

// Hard, weight 1: minQueueTimeCost — minimum hold time that is missing
// between consecutive route steps. Raw cost per lot: for each consecutive
// pair of assigned steps, max(0, previous min_queue_time - (next start -
// previous end)). With min_queue_time 0 this also charges a route overlap,
// which lotRoutePrecedenceCost counts too (kept from 0.1.x). 0 = every hold
// time is respected.
struct MinQueueTimeCost final : ScheduleViewConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;
    static constexpr Term kTerm = Term::MinQueueTimeCost;

    explicit MinQueueTimeCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("minQueueTimeCost", weight, kDefaultLevel, kTerm)
    {
    }

    static std::int64_t lotCost(const SemiconductorSchedule &schedule, int lot,
                                const ScorePlacement *const *route,
                                std::vector<std::int64_t> *items)
    {
        const Lot &l = schedule.lots[static_cast<std::size_t>(lot)];
        std::int64_t total = 0;
        for (std::size_t i = 1; i < l.operations.size(); ++i)
        {
            if (route[i - 1] == nullptr || route[i] == nullptr)
                continue;
            const Time wait = route[i]->start - route[i - 1]->end;
            total += terms::emit(items, std::max<Time>(0, l.operations[i - 1].minQueueTime - wait));
        }
        return total;
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_MIN_QUEUE_TIME_COST_HPP

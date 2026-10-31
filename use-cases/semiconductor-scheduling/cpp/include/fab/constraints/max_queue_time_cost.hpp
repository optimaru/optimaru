#ifndef FAB_CONSTRAINTS_MAX_QUEUE_TIME_COST_HPP
#define FAB_CONSTRAINTS_MAX_QUEUE_TIME_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab
{

// Hard, weight 1: maxQueueTimeCost — waiting time beyond a route step's
// maximum queue time. Raw cost per lot: for each consecutive pair of assigned
// steps whose earlier step sets max_queue_time,
// max(0, (next start - previous end) - max_queue_time). 0 = every lot moves on
// in time.
struct MaxQueueTimeCost final : ScheduleViewConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;
    static constexpr Term kTerm = Term::MaxQueueTimeCost;

    explicit MaxQueueTimeCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("maxQueueTimeCost", weight, kDefaultLevel, kTerm)
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
            const std::optional<Time> &limit = l.operations[i - 1].maxQueueTime;
            if (!limit)
                continue;
            const Time wait = route[i]->start - route[i - 1]->end;
            total += terms::emit(items, std::max<Time>(0, wait - *limit));
        }
        return total;
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_MAX_QUEUE_TIME_COST_HPP

#ifndef FAB_CONSTRAINTS_MISSING_TRANSPORT_TIME_COST_HPP
#define FAB_CONSTRAINTS_MISSING_TRANSPORT_TIME_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab
{

// Hard, weight 1: missingTransportTimeCost — material-handling time that is
// missing between consecutive route steps. Raw cost per lot: for each
// consecutive pair of assigned steps, max(0, transport(previous machine's
// group, next machine's group) - (next start - previous end)); the last
// transport_times entry wins, none = 0 (so a missing entry also charges a
// route overlap, kept from 0.1.x). 0 = every transfer fits.
struct MissingTransportTimeCost final : ScheduleViewConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;
    static constexpr Term kTerm = Term::MissingTransportTimeCost;

    explicit MissingTransportTimeCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("missingTransportTimeCost", weight, kDefaultLevel, kTerm)
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
            const ScorePlacement *previous = route[i - 1];
            const ScorePlacement *current = route[i];
            if (previous == nullptr || current == nullptr)
                continue;
            const Time wait = checked::sub(current->start, previous->end);
            const Time required = schedule.transportTime(
                schedule.machines[static_cast<std::size_t>(previous->machine)].groupIndex,
                schedule.machines[static_cast<std::size_t>(current->machine)].groupIndex);
            total = checked::add(total, terms::emit(items, std::max<Time>(0, checked::sub(required, wait))));
        }
        return total;
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_MISSING_TRANSPORT_TIME_COST_HPP

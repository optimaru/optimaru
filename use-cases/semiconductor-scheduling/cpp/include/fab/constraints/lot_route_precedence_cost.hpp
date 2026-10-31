#ifndef FAB_CONSTRAINTS_LOT_ROUTE_PRECEDENCE_COST_HPP
#define FAB_CONSTRAINTS_LOT_ROUTE_PRECEDENCE_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab
{

// Hard, weight 1: lotRoutePrecedenceCost — route steps that start too early.
// Raw cost per lot = max(0, release time - start of its first step) when the
// first step is assigned, plus max(0, previous end - start) for each
// consecutive pair of assigned route steps. 0 = every lot starts at or after
// its release and runs its route in order.
struct LotRoutePrecedenceCost final : ScheduleViewConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;
    static constexpr Term kTerm = Term::LotRoutePrecedenceCost;

    explicit LotRoutePrecedenceCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("lotRoutePrecedenceCost", weight, kDefaultLevel, kTerm)
    {
    }

    // One lot; route[i] is the assignment of route step i (nullptr = none).
    static std::int64_t lotCost(const SemiconductorSchedule &schedule, int lot,
                                const ScorePlacement *const *route,
                                std::vector<std::int64_t> *items)
    {
        const Lot &l = schedule.lots[static_cast<std::size_t>(lot)];
        std::int64_t total = 0;
        if (route[0] != nullptr)
            total += terms::emit(items, std::max<Time>(0, l.releaseTime - route[0]->start));
        for (std::size_t i = 1; i < l.operations.size(); ++i)
        {
            if (route[i - 1] == nullptr || route[i] == nullptr)
                continue;
            const Time wait = route[i]->start - route[i - 1]->end;
            total += terms::emit(items, std::max<Time>(0, -wait));
        }
        return total;
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_LOT_ROUTE_PRECEDENCE_COST_HPP

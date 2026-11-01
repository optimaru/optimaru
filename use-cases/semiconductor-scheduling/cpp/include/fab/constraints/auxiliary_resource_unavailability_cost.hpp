#ifndef FAB_CONSTRAINTS_AUXILIARY_RESOURCE_UNAVAILABILITY_COST_HPP
#define FAB_CONSTRAINTS_AUXILIARY_RESOURCE_UNAVAILABILITY_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab
{

// Hard, weight 1: auxiliaryResourceUnavailabilityCost — use of an auxiliary
// resource inside its unavailable windows. Raw cost per resource = the sum
// over the assignments using it (one entry per listing) and its windows of
// the overlap time. 0 = no resource is used while it is unavailable.
struct AuxiliaryResourceUnavailabilityCost final : ScheduleViewConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;
    static constexpr Term kTerm = Term::AuxiliaryResourceUnavailabilityCost;

    explicit AuxiliaryResourceUnavailabilityCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("auxiliaryResourceUnavailabilityCost", weight, kDefaultLevel, kTerm)
    {
    }

    static std::int64_t resourceCost(const SemiconductorSchedule &schedule, int resource,
                                     const PlacementList &users,
                                     std::vector<std::int64_t> *items)
    {
        const AuxiliaryResource &r = schedule.auxiliaryResources[static_cast<std::size_t>(resource)];
        std::int64_t total = 0;
        for (const ScorePlacement *p : users)
        {
            for (const TimeWindow &window : r.unavailable)
                total = checked::add(total, terms::emit(items, terms::overlap(p->start, p->end, window.start, window.end)));
        }
        return total;
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_AUXILIARY_RESOURCE_UNAVAILABILITY_COST_HPP

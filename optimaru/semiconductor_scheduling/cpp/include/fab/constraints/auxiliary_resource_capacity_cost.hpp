#ifndef FAB_CONSTRAINTS_AUXILIARY_RESOURCE_CAPACITY_COST_HPP
#define FAB_CONSTRAINTS_AUXILIARY_RESOURCE_CAPACITY_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab {

// Hard, weight 1: auxiliaryResourceCapacityCost — concurrent use above an
// operator, reticle, fixture or transporter capacity. Raw cost per auxiliary
// resource = the time-integrated excess over its capacity of the assignments
// whose operation requires it, one entry per listing (an operation listing a
// resource twice uses it twice). 0 = no resource is ever over-used.
struct AuxiliaryResourceCapacityCost final : ScheduleViewConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;
    static constexpr Term kTerm = Term::AuxiliaryResourceCapacityCost;

    explicit AuxiliaryResourceCapacityCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("auxiliaryResourceCapacityCost", weight, kDefaultLevel, kTerm) {}

    // One resource; `users` holds one entry per listing.
    static std::int64_t resourceCost(const SemiconductorSchedule& schedule, int resource,
                                     const PlacementList& users, std::vector<std::int64_t>* items) {
        const AuxiliaryResource& r =
                schedule.auxiliaryResources[static_cast<std::size_t>(resource)];
        return terms::emit(items, terms::overloadArea(users, r.capacity));
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_AUXILIARY_RESOURCE_CAPACITY_COST_HPP

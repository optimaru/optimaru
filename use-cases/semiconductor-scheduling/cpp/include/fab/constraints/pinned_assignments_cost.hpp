#ifndef FAB_CONSTRAINTS_PINNED_ASSIGNMENTS_COST_HPP
#define FAB_CONSTRAINTS_PINNED_ASSIGNMENTS_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab
{

// Hard, weight 1: pinnedAssignmentsCost — frozen operations that moved.
// Raw cost per pinned assignment = 1 when its operation has no assignment or
// one on another machine, start or end (the batch id is ignored); else 0.
// 0 = every pin is kept. (The decoder places pins as given, so only a seed
// or a hand-made assignment list can break one.)
struct PinnedAssignmentsCost final : ScheduleViewConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;
    static constexpr Term kTerm = Term::PinnedAssignmentsCost;

    explicit PinnedAssignmentsCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("pinnedAssignmentsCost", weight, kDefaultLevel, kTerm)
    {
    }

    // One pin (index into pinnedAssignments); `actual` = its operation's
    // assignment (nullptr = none).
    static std::int64_t pinCost(const SemiconductorSchedule &schedule, int pin,
                                const ScorePlacement *actual)
    {
        const Assignment &expected = schedule.pinnedAssignments[static_cast<std::size_t>(pin)];
        return actual == nullptr || actual->machine != expected.machine
                       || actual->start != expected.start || actual->end != expected.end
                   ? 1
                   : 0;
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_PINNED_ASSIGNMENTS_COST_HPP

#ifndef FAB_CONSTRAINTS_WEIGHTED_TARDINESS_COST_HPP
#define FAB_CONSTRAINTS_WEIGHTED_TARDINESS_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab
{

// Soft, weight 10: weightedTardinessCost — lot tardiness multiplied by its
// business priority. Raw cost per lot with its last route step assigned =
// priority * max(0, last end - due time); the sum is also the schedule's
// total_weighted_tardiness. 0 = every lot is on time.
struct WeightedTardinessCost final : ScheduleViewConstraint
{
    static constexpr double kDefaultWeight = 10.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;
    static constexpr Term kTerm = Term::WeightedTardinessCost;

    explicit WeightedTardinessCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("weightedTardinessCost", weight, kDefaultLevel, kTerm)
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
        return terms::emit(items, checked::mul(std::max<Time>(0, checked::sub(last->end, l.dueTime)), l.priority));
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_WEIGHTED_TARDINESS_COST_HPP

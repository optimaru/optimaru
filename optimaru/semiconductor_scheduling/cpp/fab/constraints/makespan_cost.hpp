#ifndef FAB_CONSTRAINTS_MAKESPAN_COST_HPP
#define FAB_CONSTRAINTS_MAKESPAN_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab {

// Soft, weight 1: makespanCost — completion time of the schedule. One raw
// cost for the whole schedule = the latest end among the lots' LAST route
// steps (not the latest end of any assignment), 0 when no lot has its last
// step assigned; it is also the schedule's makespan. 0 = nothing is
// completed (a lower bound of the soft score).
struct MakespanCost final : ScheduleViewConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;
    static constexpr Term kTerm = Term::MakespanCost;

    explicit MakespanCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("makespanCost", weight, kDefaultLevel, kTerm) {}

    // The completion times of the lots whose last step is assigned.
    static std::int64_t scheduleCost(const std::vector<Time>& completions,
                                     std::vector<std::int64_t>* items) {
        Time latest = 0;
        bool any = false;
        for (Time completion : completions) {
            latest = any ? std::max(latest, completion) : completion;
            any = true;
        }
        return terms::emit(items, any ? latest : 0);
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_MAKESPAN_COST_HPP

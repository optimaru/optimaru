#ifndef FAB_CONSTRAINTS_MACHINE_LOAD_IMBALANCE_COST_HPP
#define FAB_CONSTRAINTS_MACHINE_LOAD_IMBALANCE_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab
{

// Soft, weight 1: machineLoadImbalanceCost — processing-load range between
// the alternative tools of an equipment group. Raw cost per machine group
// with at least two machines (first-appearance order) = max load - min load,
// a machine's load being the summed length of its assignments. 0 = every
// group is evenly loaded.
struct MachineLoadImbalanceCost final : ScheduleViewConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;
    static constexpr Term kTerm = Term::MachineLoadImbalanceCost;

    explicit MachineLoadImbalanceCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("machineLoadImbalanceCost", weight, kDefaultLevel, kTerm)
    {
    }

    // A machine's load: the summed length of its assignments.
    static std::int64_t machineLoad(const PlacementList &rows)
    {
        std::int64_t load = 0;
        for (const ScorePlacement *p : rows)
            load += p->end - p->start;
        return load;
    }
    // One group: the loads of its machines in machine order.
    static std::int64_t groupCost(const std::vector<std::int64_t> &loads,
                                  std::vector<std::int64_t> *items)
    {
        if (loads.size() < 2)
            return 0;
        const auto [low, high] = std::minmax_element(loads.begin(), loads.end());
        return terms::emit(items, *high - *low);
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_MACHINE_LOAD_IMBALANCE_COST_HPP

#ifndef TA_CONSTRAINTS_SQUARED_MAKESPAN_COST_HPP
#define TA_CONSTRAINTS_SQUARED_MAKESPAN_COST_HPP

#include "ta/constraints/common.hpp"

namespace ta
{

// Soft, weight kMakespanUnit (10^7): squaredMakespanCost — every employee
// penalizes the SQUARE of its end time, which is what pushes work off the
// busiest employee instead of merely shortening the total. Raw cost per
// employee = (end time of the last task)^2 in minutes^2, so 0 = an employee
// without tasks (it ends at 0). The Bendable soft[1] level: the default
// weight 10^7 carries it above the priority end times (soft[2]) on the one
// soft level, the packing the Score adapter decodes (domain.hpp).
//
// The squaring lives in cost() rather than in `exponent` on purpose: the
// exponent shapes each unit's raw cost before the weight; here the square is
// the constraint's own definition, so a caller can still shape it further.
struct SquaredMakespanCost final : TaUnitConstraint
{
    static constexpr double kDefaultWeight = static_cast<double>(kMakespanUnit);
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit SquaredMakespanCost(double weight = kDefaultWeight)
        : TaUnitConstraint("squaredMakespanCost", weight, kDefaultLevel)
    {
    }
    double cost(const TaskAssigningSolution &solution,
                int unit,
                const std::vector<int> &members) const override
    {
        const Minutes endTime
            = detail::walkChain(solution, unit, members, [](int, Minutes) {});
        return static_cast<double>(endTime * endTime);
    }
};

}  // namespace ta

#endif  // TA_CONSTRAINTS_SQUARED_MAKESPAN_COST_HPP

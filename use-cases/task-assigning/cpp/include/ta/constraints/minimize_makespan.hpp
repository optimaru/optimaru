#ifndef TA_CONSTRAINTS_MINIMIZE_MAKESPAN_HPP
#define TA_CONSTRAINTS_MINIMIZE_MAKESPAN_HPP

#include "ta/constraints/common.hpp"

namespace ta
{

// Soft (default, weight kMakespanUnit): minimizeMakespan — every employee
// penalizes the SQUARE of its end time, which is what pushes work off the
// busiest employee instead of merely shortening the total. The Bendable
// soft[1] level, carried above the priority end times by the unit factor
// (see common.hpp). An employee without tasks ends at 0 and costs nothing.
//
// The squaring lives in cost() rather than in `exponent` on purpose: the
// exponent shapes the WEIGHTED cost of a unit, and here the square is the
// constraint's own definition, so a caller can still shape it further.
struct MinimizeMakespan final : TaUnitConstraint
{
    explicit MinimizeMakespan(double weight
                              = static_cast<double>(kMakespanUnit))
        : TaUnitConstraint("minimizeMakespan", weight, /*hard=*/false)
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

#endif  // TA_CONSTRAINTS_MINIMIZE_MAKESPAN_HPP

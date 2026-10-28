#ifndef BA_CONSTRAINTS_SAME_BED_IN_SAME_NIGHT_HPP
#define BA_CONSTRAINTS_SAME_BED_IN_SAME_NIGHT_HPP

#include "ba/constraints/common.hpp"

namespace ba
{

// Hard (default, weight 1000): sameBedInSameNight — one point per night two
// stays share the same bed. A sum over the pairs INSIDE one unit, so the
// exact delta of one stay joining or leaving is its overlap with the other
// occupants.
struct SameBedInSameNight final : BaUnitConstraint
{
    explicit SameBedInSameNight(double weight = 1000.0)
        : BaUnitConstraint("sameBedInSameNight", weight, /*hard=*/true)
    {
    }
    void onReset(const BedPlan &plan) override
    {
        detail::requirePrepared(plan);
    }
    double cost(const BedPlan &plan,
                int unit,
                const std::vector<int> &members) const override
    {
        if (!detail::isRealBed(plan, unit))
            return 0.0;
        std::int64_t nights = 0;
        for (std::size_t a = 0; a < members.size(); ++a)
        {
            const Stay &first = plan.stays[members[a]];
            for (std::size_t b = a + 1; b < members.size(); ++b)
                nights += first.sameNightCount(plan.stays[members[b]]);
        }
        return static_cast<double>(nights);
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const BedPlan &plan,
                     int unit,
                     const std::vector<int> &members,
                     int entity) const override
    {
        if (!detail::isRealBed(plan, unit))
            return 0.0;
        const Stay &stay = plan.stays[entity];
        std::int64_t nights = 0;
        for (int other : members)
        {
            if (other != entity)
                nights += stay.sameNightCount(plan.stays[other]);
        }
        return static_cast<double>(nights);
    }
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_SAME_BED_IN_SAME_NIGHT_HPP

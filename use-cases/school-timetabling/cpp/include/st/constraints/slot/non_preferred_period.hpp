#ifndef ST_CONSTRAINTS_SLOT_NON_PREFERRED_PERIOD_HPP
#define ST_CONSTRAINTS_SLOT_NON_PREFERRED_PERIOD_HPP

#include "st/constraints/slot/slot_constraint.hpp"

namespace st::canon
{

// Soft (default, weight = ObjectiveWeights::nonPreferredPeriod): S6 — one
// point per preference unit with a non-empty preferred mask that does NOT
// contain the slot's period.
struct NonPreferredPeriod final : GroupableSlotConstraint
{
    explicit NonPreferredPeriod(double weight
                                = ObjectiveWeights{}.nonPreferredPeriod)
        : GroupableSlotConstraint("S6 non-preferred period",
                                  weight,
                                  /*hard=*/false)
    {
    }
    void onReset(const Timetable &table) override
    {
        detail::requirePrepared(table);
    }
    double cost(const Timetable &table,
                int unit,
                const std::vector<int> &members) const override
    {
        const std::uint16_t bit = detail::periodBit(table, unit);
        std::int64_t hits = 0;
        for (int i : members)
        {
            for (const auto &[avoided, preferred] :
                 table.lessons[i].preferenceUnits)
            {
                (void)avoided;
                if (preferred != 0 && (preferred & bit) == 0)
                    ++hits;
            }
        }
        return static_cast<double>(hits);
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const Timetable &table,
                     int unit,
                     const std::vector<int> & /*members*/,
                     int entity) const override
    {
        const std::uint16_t bit = detail::periodBit(table, unit);
        std::int64_t hits = 0;
        for (const auto &[avoided, preferred] :
             table.lessons[entity].preferenceUnits)
        {
            (void)avoided;
            if (preferred != 0 && (preferred & bit) == 0)
                ++hits;
        }
        return static_cast<double>(hits);
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_SLOT_NON_PREFERRED_PERIOD_HPP

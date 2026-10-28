#ifndef ST_CONSTRAINTS_SLOT_AVOIDED_PERIOD_HPP
#define ST_CONSTRAINTS_SLOT_AVOIDED_PERIOD_HPP

#include "st/constraints/slot/slot_constraint.hpp"

namespace st::canon
{

// Soft (default, weight = ObjectiveWeights::avoidedPeriod): S5 — one point
// per preference unit whose avoided-period mask contains the slot's period
// (a merged lesson counts once per member course, like validate.py).
struct AvoidedPeriod final : GroupableSlotConstraint
{
    explicit AvoidedPeriod(double weight = ObjectiveWeights{}.avoidedPeriod)
        : GroupableSlotConstraint("S5 avoided period", weight, /*hard=*/false)
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
                (void)preferred;
                if ((avoided & bit) != 0)
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
            (void)preferred;
            if ((avoided & bit) != 0)
                ++hits;
        }
        return static_cast<double>(hits);
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_SLOT_AVOIDED_PERIOD_HPP

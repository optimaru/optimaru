#ifndef ST_CONSTRAINTS_SLOT_AVOIDED_PERIOD_COST_HPP
#define ST_CONSTRAINTS_SLOT_AVOIDED_PERIOD_COST_HPP

#include "st/constraints/slot/slot_constraint.hpp"

namespace st::canon
{

// avoidedPeriodCost — soft, weight 5. Raw cost per slot = one point per
// preference unit of its lessons whose subject avoids the slot's period
// (avoided_periods; a merged lesson counts once per member course, like
// validate.py); 0 = no lesson sits in an avoided period.
struct AvoidedPeriodCost final : GroupableSlotConstraint
{
    static constexpr double kDefaultWeight = 5.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit AvoidedPeriodCost(double weight = kDefaultWeight)
        : GroupableSlotConstraint("avoidedPeriodCost", weight, kDefaultLevel)
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

#endif  // ST_CONSTRAINTS_SLOT_AVOIDED_PERIOD_COST_HPP

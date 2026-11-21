#ifndef ST_CONSTRAINTS_SLOT_NON_PREFERRED_PERIOD_COST_HPP
#define ST_CONSTRAINTS_SLOT_NON_PREFERRED_PERIOD_COST_HPP

#include "st/constraints/slot/slot_constraint.hpp"

namespace st::canon {

// nonPreferredPeriodCost — soft, weight 1. Raw cost per slot = one point per
// preference unit of its lessons whose subject has preferred_periods that do
// NOT contain the slot's period (a subject without preferences never
// counts); 0 = every lesson with a preference sits in a preferred period.
struct NonPreferredPeriodCost final : GroupableSlotConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit NonPreferredPeriodCost(double weight = kDefaultWeight)
        : GroupableSlotConstraint("nonPreferredPeriodCost", weight, kDefaultLevel) {}
    void on_reset(const Timetable& table) override { detail::requirePrepared(table); }
    double cost(const Timetable& table, int unit, const std::vector<int>& members) const override {
        const std::uint16_t bit = detail::periodBit(table, unit);
        std::int64_t hits = 0;
        for (int i : members) {
            for (const auto& [avoided, preferred] : table.lessons[i].preferenceUnits) {
                (void)avoided;
                if (preferred != 0 && (preferred & bit) == 0) ++hits;
            }
        }
        return static_cast<double>(hits);
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const Timetable& table, int unit, const std::vector<int>& /*members*/,
                      int entity) const override {
        const std::uint16_t bit = detail::periodBit(table, unit);
        std::int64_t hits = 0;
        for (const auto& [avoided, preferred] : table.lessons[entity].preferenceUnits) {
            (void)avoided;
            if (preferred != 0 && (preferred & bit) == 0) ++hits;
        }
        return static_cast<double>(hits);
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_SLOT_NON_PREFERRED_PERIOD_COST_HPP

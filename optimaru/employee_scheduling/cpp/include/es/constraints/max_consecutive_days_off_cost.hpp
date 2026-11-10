#ifndef ES_CONSTRAINTS_MAX_CONSECUTIVE_DAYS_OFF_COST_HPP
#define ES_CONSTRAINTS_MAX_CONSECUTIVE_DAYS_OFF_COST_HPP

#include "es/constraints/common.hpp"

namespace es {

// Soft, weight 1: maxConsecutiveDaysOffCost. Interior runs of consecutive
// OFF days (days with no work) longer than `value` (default 3). An OFF run is
// counted only when it sits strictly between two worked days (bounded by
// work on both sides), so leading/trailing off at the horizon edge — likely
// an artifact of the planning window — is not penalized. Raw cost per
// employee = len - value per over-long interior run ("avoid long gaps between
// worked days": value=3 -> runs of 4+ cost), so 0 = no such gap. A day counts
// as worked if it holds any shift. Runs without delta_cost (full unit re-cost
// per touch).
struct MaxConsecutiveDaysOffCost final : EsUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit MaxConsecutiveDaysOffCost(double weight = kDefaultWeight, int value = 3)
        : EsUnitConstraint("maxConsecutiveDaysOffCost", weight, kDefaultLevel), value_(value) {}
    void on_reset(const EmployeeSchedule& s) override {
        horizon_ = 0;
        day_.resize(s.shifts.size());
        for (std::size_t i = 0; i < s.shifts.size(); ++i) {
            day_[i] = static_cast<std::int32_t>(s.shifts[i].startDay);
            horizon_ = std::max(horizon_, day_[i] + 1);
        }
        worked_.assign(horizon_, 0);
    }
    double cost(const EmployeeSchedule&, int /*unit*/,
                const std::vector<int>& members) const override {
        for (int i : members) worked_[day_[i]] = 1;
        std::int32_t first = -1, last = -1;
        for (std::int32_t d = 0; d < horizon_; ++d)
            if (worked_[d]) {
                if (first < 0) first = d;
                last = d;
            }
        double c = 0;
        std::int32_t run = 0;  // consecutive off days between first..last (interior)
        for (std::int32_t d = first; d >= 0 && d <= last; ++d) {
            if (worked_[d]) {
                if (run > value_) c += run - value_;
                run = 0;
            } else {
                ++run;
            }
        }
        for (int i : members) worked_[day_[i]] = 0;  // restore the scratch
        return c;
    }

private:
    std::int32_t value_;
    std::vector<std::int32_t> day_;     // shift -> start day
    mutable std::vector<char> worked_;  // scratch, zeroed after use
    std::int32_t horizon_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_MAX_CONSECUTIVE_DAYS_OFF_COST_HPP

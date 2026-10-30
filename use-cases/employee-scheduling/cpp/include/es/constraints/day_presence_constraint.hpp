#ifndef ES_CONSTRAINTS_DAY_PRESENCE_CONSTRAINT_HPP
#define ES_CONSTRAINTS_DAY_PRESENCE_CONSTRAINT_HPP

#include "es/constraints/common.hpp"

namespace es {

// Shared base: per-employee day-presence statistics ("worked days"). A day
// counts once even with several shifts (day = the shift's start day).
// Subclasses read the run-length structure.
struct DayPresenceConstraint : EsUnitConstraint {
    DayPresenceConstraint(std::string name, double weight, bool hard)
        : EsUnitConstraint(std::move(name), weight, hard) {}
    void onReset(const EmployeeSchedule& s) override {
        horizon_ = 0;
        day_.resize(s.shifts.size());
        for (std::size_t i = 0; i < s.shifts.size(); ++i) {
            day_[i] = static_cast<std::int32_t>(s.shifts[i].startDay);
            horizon_ = std::max(horizon_, day_[i] + 1);
        }
        worked_.assign(horizon_, 0);
        pinned_.assign(horizon_, 0);
    }
    double cost(const EmployeeSchedule& s, int /*unit*/,
                const std::vector<int>& members) const override {
        for (int i : members) {
            worked_[day_[i]] = 1;
            if (s.shifts[i].pinned) pinned_[day_[i]] = 1;
        }
        double c = 0;
        std::int32_t run = 0;
        bool allPinned = true;  // whole current run sits on pinned (fixed) shifts?
        for (std::int32_t d = 0; d < horizon_; ++d) {
            if (worked_[d]) {
                ++run;
                if (!pinned_[d]) allPinned = false;
            } else if (run > 0) {
                if (!allPinned) c += runCost(run, /*touchesEnd=*/false);  // B: skip pinned-only run
                run = 0;
                allPinned = true;
            }
        }
        if (run > 0 && !allPinned) c += runCost(run, /*touchesEnd=*/true);
        for (int i : members) {
            worked_[day_[i]] = 0;
            pinned_[day_[i]] = 0;  // restore the scratch
        }
        return c;
    }
    // Cost of one maximal run of `len` consecutive worked days. `touchesEnd` is
    // true when the run reaches the horizon's last day (its future is unknown —
    // an at-least/shortfall rule waives it; an at-most/excess rule still counts it).
    virtual double runCost(std::int32_t len, bool touchesEnd) const = 0;

protected:
    std::vector<std::int32_t> day_;       // shift -> start day
    mutable std::vector<char> worked_;    // scratch, always zeroed after use
    mutable std::vector<char> pinned_;    // scratch: day sits on a pinned shift
    std::int32_t horizon_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_DAY_PRESENCE_CONSTRAINT_HPP

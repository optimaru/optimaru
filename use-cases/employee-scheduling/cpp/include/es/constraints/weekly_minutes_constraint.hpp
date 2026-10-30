#ifndef ES_CONSTRAINTS_WEEKLY_MINUTES_CONSTRAINT_HPP
#define ES_CONSTRAINTS_WEEKLY_MINUTES_CONSTRAINT_HPP

#include "es/constraints/common.hpp"

namespace es {

// Shared base: per-employee minutes per calendar week (7-day blocks from day
// 0; a shift's whole duration counts in its start week).
struct WeeklyMinutesConstraint : EsUnitConstraint {
    WeeklyMinutesConstraint(std::string name, double weight, bool hard, Minutes minutes,
                            int weekAnchor = 0)
        : EsUnitConstraint(std::move(name), weight, hard),
          threshold_(static_cast<std::int64_t>(minutes)),
          weekAnchor_(weekAnchor) {}
    void onReset(const EmployeeSchedule& s) override {
        const std::size_t n = s.shifts.size();
        week_.resize(n);
        duration_.resize(n);
        // Week index = floor((startDay - weekAnchor) / 7), then shifted so the
        // smallest is 0. weekAnchor aligns the 7-day blocks to a calendar week
        // boundary (an engine day that starts a week); default 0 keeps the
        // plain day-0-anchored blocks. Floor division (not C++ truncation)
        // so days before the anchor land in the correct earlier week.
        std::vector<std::int32_t> raw(n);
        std::int32_t minRaw = 0, maxRaw = 0;
        bool any = false;
        for (std::size_t i = 0; i < n; ++i) {
            const std::int32_t x = static_cast<std::int32_t>(s.shifts[i].startDay) - weekAnchor_;
            const std::int32_t r = (x >= 0) ? x / 7 : -((-x + 6) / 7);
            raw[i] = r;
            duration_[i] = static_cast<std::int32_t>(s.shifts[i].end - s.shifts[i].start);
            if (!any) {
                minRaw = maxRaw = r;
                any = true;
            } else {
                minRaw = std::min(minRaw, r);
                maxRaw = std::max(maxRaw, r);
            }
        }
        weeks_ = any ? (maxRaw - minRaw + 1) : 0;
        for (std::size_t i = 0; i < n; ++i) week_[i] = raw[i] - minRaw;
        total_.assign(weeks_, 0);
        movable_.assign(weeks_, 0);
        pinned_.assign(weeks_, 0);
    }
    double cost(const EmployeeSchedule& s, int /*unit*/,
                const std::vector<int>& members) const override {
        for (int i : members) {
            total_[week_[i]] += duration_[i];
            (s.shifts[i].pinned ? pinned_ : movable_)[week_[i]] = 1;
        }
        double c = 0;
        for (std::int32_t w = 0; w < weeks_; ++w)
            // B: skip a week whose shifts are all pinned (fixed history); an idle
            // week (no shift at all) is still scored so guaranteed-hours holds.
            if (!(pinned_[w] && !movable_[w])) c += weekCost(total_[w]);
        for (int i : members) { total_[week_[i]] = 0; movable_[week_[i]] = 0; pinned_[week_[i]] = 0; }
        return c;
    }
    virtual double weekCost(std::int64_t totalMinutes) const = 0;

protected:
    std::int64_t threshold_;
    int weekAnchor_;
    std::vector<std::int32_t> week_, duration_;  // per shift
    mutable std::vector<std::int64_t> total_;    // scratch, zeroed after use
    mutable std::vector<char> movable_, pinned_;  // scratch: week has movable / pinned shift
    std::int32_t weeks_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_WEEKLY_MINUTES_CONSTRAINT_HPP

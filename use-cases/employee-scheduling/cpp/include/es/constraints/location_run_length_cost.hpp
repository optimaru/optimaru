#ifndef ES_CONSTRAINTS_LOCATION_RUN_LENGTH_COST_HPP
#define ES_CONSTRAINTS_LOCATION_RUN_LENGTH_COST_HPP

#include "es/constraints/location_presence_base.hpp"

namespace es {

// Soft, weight 1: locationRunLengthCost, consecutive-run bounds on a
// location (empty = any work). Each maximal run of `minRun..maxRun`
// consecutive days holding the location is fine; raw cost per employee = the
// shortfall of each shorter run plus the excess of each longer one (maxRun <
// 0 = no upper bound), so 0 = every run is within bounds (the defaults
// 0..inf never cost anything). Example: ("Location B", 3, 6): forbid runs
// shorter than 3 days or longer than 6 at Location B. ("", 0, 6) soft:
// discourage 7+ days of any work in a row. Each instance carries the label
// "Location run[<location|work> <min>..<max|inf>]".
struct LocationRunLengthCost final : LocationPresenceBase {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit LocationRunLengthCost(double weight = kDefaultWeight, std::string location = {},
                                   int minRun = 0, int maxRun = -1, bool waiveMinAtEnd = true)
        : LocationPresenceBase("locationRunLengthCost", weight, kDefaultLevel, std::move(location)),
          min_(minRun), max_(maxRun), waiveMinAtEnd_(waiveMinAtEnd) {
        label = "Location run[" + (loc_.empty() ? std::string("work") : loc_) + " " +
                std::to_string(min_) + ".." + (max_ < 0 ? std::string("inf") : std::to_string(max_)) +
                "]";  // display only; never a lookup key
    }
    double cost(const EmployeeSchedule& s, int, const std::vector<int>& members) const override {
        for (int i : members)
            if (match_[i]) {
                present_[day_[i]] = 1;
                if (s.shifts[i].pinned) pin_[day_[i]] = 1;
            }
        double c = 0;
        std::int32_t run = 0;
        bool allPinned = true;
        for (std::int32_t d = 0; d < horizon_; ++d) {
            if (present_[d]) {
                ++run;
                if (!pin_[d]) allPinned = false;
            } else if (run > 0) {
                if (!allPinned) c += runCost(run, /*touchesEnd=*/false);
                run = 0;
                allPinned = true;
            }
        }
        if (run > 0 && !allPinned) c += runCost(run, /*touchesEnd=*/true);
        for (int i : members)
            if (match_[i]) { present_[day_[i]] = 0; pin_[day_[i]] = 0; }
        return c;
    }

private:
    // B: pinned-only runs are skipped by the caller. C: a run reaching the end of
    // the horizon may waive its shortfall (min side) — it can continue past the
    // schedule — but only when waiveMinAtEnd_ is set. A caller whose rule stays
    // in force past the last scheduled day passes waiveMinAtEnd_=false: the day
    // after the last shift then CLOSES the run, so an end run of length < min IS
    // a violation. Excess (max side) always counts regardless.
    double runCost(std::int32_t len, bool touchesEnd) const {
        double c = 0;
        if (!(touchesEnd && waiveMinAtEnd_) && len < min_) c += min_ - len;
        if (max_ >= 0 && len > max_) c += len - max_;
        return c;
    }
    int min_, max_;
    bool waiveMinAtEnd_;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_LOCATION_RUN_LENGTH_COST_HPP

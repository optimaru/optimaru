#ifndef ES_CONSTRAINTS_LOCATION_RUN_HPP
#define ES_CONSTRAINTS_LOCATION_RUN_HPP

#include "es/constraints/location_presence_base.hpp"

namespace es {

// Consecutive-run bounds on a location: each maximal run of `minRun..maxRun`
// consecutive days holding the location is fine; a shorter run costs its
// shortfall, a longer run its excess (maxRun < 0 = no upper bound). Example:
// ("Location B", 3, 6): forbid runs shorter than 3 days or longer than 6 at
// Location B. ("", 0, 6) soft: discourage 7+ days of any work in a row.
struct LocationRun final : LocationPresenceBase {
    LocationRun(double weight, std::string location, int minRun, int maxRun,
                bool waiveMinAtEnd = true)
        : LocationPresenceBase("Location run[" + (location.empty() ? "work" : location) + " " +
                                       std::to_string(minRun) + ".." +
                                       (maxRun < 0 ? std::string("inf") : std::to_string(maxRun)) +
                                       "]",
                               weight, /*hard=*/false, std::move(location)),
          min_(minRun), max_(maxRun), waiveMinAtEnd_(waiveMinAtEnd) {}
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

#endif  // ES_CONSTRAINTS_LOCATION_RUN_HPP

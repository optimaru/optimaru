#ifndef ES_CONSTRAINTS_LOCATION_PRESENCE_BASE_HPP
#define ES_CONSTRAINTS_LOCATION_PRESENCE_BASE_HPP

#include "es/constraints/common.hpp"

namespace es {

// Shared helper base: builds per-shift (day, location-match) tables and a
// per-day presence scratch for one target location.
struct LocationPresenceBase : EsUnitConstraint {
    LocationPresenceBase(std::string name, double weight, bool hard, std::string location)
        : EsUnitConstraint(std::move(name), weight, hard), loc_(std::move(location)) {}
    void onReset(const EmployeeSchedule& s) override {
        horizon_ = 0;
        day_.resize(s.shifts.size());
        match_.resize(s.shifts.size());
        for (std::size_t i = 0; i < s.shifts.size(); ++i) {
            day_[i] = static_cast<std::int32_t>(s.shifts[i].startDay);
            match_[i] = (loc_.empty() || s.shifts[i].location == loc_) ? 1 : 0;
            horizon_ = std::max(horizon_, day_[i] + 1);
        }
        present_.assign(horizon_, 0);
        work_.assign(horizon_, 0);
        pin_.assign(horizon_, 0);
    }

protected:
    std::string loc_;
    std::vector<std::int32_t> day_;
    std::vector<char> match_;              // shift -> matches target location
    mutable std::vector<char> present_;    // scratch: day has the target location
    mutable std::vector<char> work_;       // scratch: day has any work
    mutable std::vector<char> pin_;        // scratch: the matching shift on the day is pinned
    std::int32_t horizon_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_LOCATION_PRESENCE_BASE_HPP

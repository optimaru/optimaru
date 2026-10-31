#ifndef ES_CONSTRAINTS_REST_AFTER_LOCATION_RUN_HPP
#define ES_CONSTRAINTS_REST_AFTER_LOCATION_RUN_HPP

#include "es/constraints/location_presence_base.hpp"

namespace es {

// After a maximal run of the trigger location ends, the next `restDays` days
// must be fully OFF (no work of any kind). Penalty = worked days within that
// required rest window. Example: ("Location B", 3): after any stint at Location B,
// the following 3 days must be off.
struct RestAfterLocationRun final : LocationPresenceBase {
    RestAfterLocationRun(double weight, std::string location, int restDays)
        : LocationPresenceBase("Rest after location run[" +
                                       (location.empty() ? "work" : location) + " " +
                                       std::to_string(restDays) + "]",
                               weight, /*hard=*/true, std::move(location)),
          rest_(restDays) {}
    double cost(const EmployeeSchedule&, int, const std::vector<int>& members) const override {
        for (int i : members) {
            work_[day_[i]] = 1;
            if (match_[i]) present_[day_[i]] = 1;
        }
        double c = 0;
        for (std::int32_t d = 0; d < horizon_; ++d) {
            // A location-run ends at day d if d holds it and d+1 does not.
            const bool runEnds = present_[d] && (d + 1 >= horizon_ || !present_[d + 1]);
            if (runEnds) {
                for (std::int32_t k = 1; k <= rest_ && d + k < horizon_; ++k)
                    if (work_[d + k]) c += 1;  // a day that should be off but is worked
            }
        }
        for (int i : members) {
            work_[day_[i]] = 0;
            if (match_[i]) present_[day_[i]] = 0;
        }
        return c;
    }

private:
    int rest_;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_REST_AFTER_LOCATION_RUN_HPP

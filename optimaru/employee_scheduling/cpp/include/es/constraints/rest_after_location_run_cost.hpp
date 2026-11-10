#ifndef ES_CONSTRAINTS_REST_AFTER_LOCATION_RUN_COST_HPP
#define ES_CONSTRAINTS_REST_AFTER_LOCATION_RUN_COST_HPP

#include "es/constraints/location_presence_base.hpp"

namespace es {

// Hard, weight 1: restAfterLocationRunCost. After a maximal run of the
// trigger location (empty = any work) ends, the next `restDays` (default 1)
// days must be fully OFF (no work of any kind). Raw cost per employee = the
// worked days within those required rest windows, so 0 = every stint is
// followed by its rest. Example: ("Location B", 3): after any stint at
// Location B, the following 3 days must be off. Each instance carries the
// label "Rest after location run[<location|work> <restDays>]".
struct RestAfterLocationRunCost final : LocationPresenceBase {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit RestAfterLocationRunCost(double weight = kDefaultWeight, std::string location = {},
                                      int restDays = 1)
        : LocationPresenceBase("restAfterLocationRunCost", weight, kDefaultLevel,
                               std::move(location)),
          rest_(restDays) {
        label = "Rest after location run[" + (loc_.empty() ? std::string("work") : loc_) + " " +
                std::to_string(rest_) + "]";  // display only; never a lookup key
    }
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

#endif  // ES_CONSTRAINTS_REST_AFTER_LOCATION_RUN_COST_HPP

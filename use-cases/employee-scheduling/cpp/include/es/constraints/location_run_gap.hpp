#ifndef ES_CONSTRAINTS_LOCATION_RUN_GAP_HPP
#define ES_CONSTRAINTS_LOCATION_RUN_GAP_HPP

#include "es/constraints/location_presence_base.hpp"

namespace es {

// Minimum day gap between two consecutive maximal runs of the location. Penalty
// = (minGap - gap) for each pair of runs whose separation is too short. Example:
// ("Location B", 10): stints at Location B should be >= 10 days apart.
struct LocationRunGap final : LocationPresenceBase {
    LocationRunGap(double weight, std::string location, int minGap)
        : LocationPresenceBase("Location run gap[" + (location.empty() ? "work" : location) + " " +
                                       std::to_string(minGap) + "]",
                               weight, /*hard=*/false, std::move(location)),
          minGap_(minGap) {}
    double cost(const EmployeeSchedule&, int, const std::vector<int>& members) const override {
        for (int i : members)
            if (match_[i]) present_[day_[i]] = 1;
        double c = 0;
        std::int32_t lastEnd = -1;  // last day of the previous run (-1 = none yet)
        std::int32_t d = 0;
        while (d < horizon_) {
            if (present_[d]) {
                std::int32_t start = d;
                while (d < horizon_ && present_[d]) ++d;
                std::int32_t end = d - 1;
                if (lastEnd >= 0) {
                    const std::int32_t gap = start - lastEnd - 1;
                    if (gap < minGap_) c += minGap_ - gap;
                }
                lastEnd = end;
            } else {
                ++d;
            }
        }
        for (int i : members)
            if (match_[i]) present_[day_[i]] = 0;
        return c;
    }

private:
    int minGap_;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_LOCATION_RUN_GAP_HPP

#ifndef ES_CONSTRAINTS_LOCATION_RUN_GAP_COST_HPP
#define ES_CONSTRAINTS_LOCATION_RUN_GAP_COST_HPP

#include "es/constraints/location_presence_base.hpp"

namespace es {

// Soft, weight 1: locationRunGapCost, the minimum day gap between two
// consecutive maximal runs of the location (empty = any work). Raw cost per
// employee = (minGap - gap) for each pair of consecutive runs whose
// separation is too short, so 0 = the stints are at least `minGap` (default
// 0: never binds) days apart. Example: ("Location B", 10): stints at Location
// B should be >= 10 days apart. Each instance carries the label
// "Location run gap[<location|work> <minGap>]".
struct LocationRunGapCost final : LocationPresenceBase {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit LocationRunGapCost(double weight = kDefaultWeight, std::string location = {},
                                int minGap = 0)
        : LocationPresenceBase("locationRunGapCost", weight, kDefaultLevel, std::move(location)),
          minGap_(minGap) {
        label = "Location run gap[" + (loc_.empty() ? std::string("work") : loc_) + " " +
                std::to_string(minGap_) + "]";  // display only; never a lookup key
    }
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

#endif  // ES_CONSTRAINTS_LOCATION_RUN_GAP_COST_HPP

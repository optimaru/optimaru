#ifndef ES_CONSTRAINTS_MINIMUM_REST_HPP
#define ES_CONSTRAINTS_MINIMUM_REST_HPP

#include "es/constraints/assignment_constraint.hpp"

namespace es {

// Hard (default): rest shortfall below `minGapMinutes` (default 10 hours)
// between two same-employee shifts. The gap threshold is a constructor
// parameter; the default matches the monolithic score calculator bit-for-bit.
struct MinimumRest final : GroupableAssignmentConstraint {
    explicit MinimumRest(double weight = 1.0,
                         Minutes minGapMinutes = ScoreCalculator::MIN_REST_MINUTES)
        : GroupableAssignmentConstraint("At least 10 hours between 2 shifts", weight,
                                        /*hard=*/true),
          minGap_(static_cast<std::int32_t>(minGapMinutes)) {}
    void onReset(const EmployeeSchedule& s) override {
        start_.resize(s.shifts.size());
        end_.resize(s.shifts.size());
        for (std::size_t i = 0; i < s.shifts.size(); ++i) {
            start_[i] = static_cast<std::int32_t>(s.shifts[i].start);
            end_[i] = static_cast<std::int32_t>(s.shifts[i].end);
        }
    }
    double cost(const EmployeeSchedule&, int /*unit*/,
                const std::vector<int>& members) const override {
        std::int64_t c = 0;
        for (std::size_t a = 0; a < members.size(); ++a) {
            for (std::size_t b = a + 1; b < members.size(); ++b) {
                c += pairShortfall(members[a], members[b]);
            }
        }
        return static_cast<double>(c);
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const EmployeeSchedule&, int /*unit*/,
                     const std::vector<int>& members, int entity) const override {
        std::int64_t c = 0;
        for (int i : members) {
            if (i != entity) c += pairShortfall(entity, i);
        }
        return static_cast<double>(c);
    }
    Minutes minGapMinutes() const { return minGap_; }

private:
    std::int32_t pairShortfall(int x, int y) const {
        const int early = (start_[x] <= start_[y]) ? x : y;
        const int late = (early == x) ? y : x;
        if (end_[early] > start_[late]) return 0;  // overlapping pair: no rest term
        const std::int32_t gap = start_[late] - end_[early];
        return gap < minGap_ ? minGap_ - gap : 0;
    }
    std::int32_t minGap_;
    std::vector<std::int32_t> start_, end_;  // dense copies of the shift times
};

}  // namespace es

#endif  // ES_CONSTRAINTS_MINIMUM_REST_HPP

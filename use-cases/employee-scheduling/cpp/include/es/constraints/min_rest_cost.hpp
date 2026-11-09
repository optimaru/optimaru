#ifndef ES_CONSTRAINTS_MIN_REST_COST_HPP
#define ES_CONSTRAINTS_MIN_REST_COST_HPP

#include "es/constraints/assignment_constraint.hpp"

namespace es {

// Hard, weight 1: minRestCost. Raw cost per employee = the rest shortfall in
// minutes below `minGapMinutes` (default 600 = 10 hours), summed over every
// pair of its non-overlapping shifts (a zero gap counts), so 0 = every two
// shifts are at least `minGapMinutes` apart. The default threshold matches
// the monolithic score calculator bit-for-bit.
struct MinRestCost final : GroupableAssignmentConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MinRestCost(double weight = kDefaultWeight,
                         Minutes minGapMinutes = ScoreCalculator::MIN_REST_MINUTES)
        : GroupableAssignmentConstraint("minRestCost", weight, kDefaultLevel),
          minGap_(static_cast<std::int32_t>(minGapMinutes)) {}
    void on_reset(const EmployeeSchedule& s) override {
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
    bool supports_delta() const override { return true; }
    double delta_cost(const EmployeeSchedule&, int /*unit*/,
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

#endif  // ES_CONSTRAINTS_MIN_REST_COST_HPP

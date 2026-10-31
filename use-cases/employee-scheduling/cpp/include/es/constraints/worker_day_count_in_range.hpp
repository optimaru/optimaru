#ifndef ES_CONSTRAINTS_WORKER_DAY_COUNT_IN_RANGE_HPP
#define ES_CONSTRAINTS_WORKER_DAY_COUNT_IN_RANGE_HPP

#include "es/constraints/common.hpp"

namespace es {

// Number of DISTINCT worked days a worker (the unit) has within a fixed,
// contiguous day range [lo, hi], bounded to [min, max] (max < 0 = no upper
// bound); shortfall/excess is the penalty. The building block for off-count
// windows: "at least K days off in this window" == "at most (windowDays - K)
// worked days", so a caller adds one of these per (worker, window) with
// max = windowLen - minOff, windows clipped to the scheduled days and the bound
// pre-adjusted for any fixed history. This only sums the planning-day
// assignments. Scope to the one worker via worker_indices (ScopedUnit). Counts
// pinned days too, though clipped ranges carry none. Example: two days off in a
// calendar week -> (lo,hi)=that week, max=len-2.
struct WorkerDayCountInRange final : EsUnitConstraint {
    WorkerDayCountInRange(double weight, int lo, int hi, int minCount, int maxCount,
                          std::string location = "")
        : EsUnitConstraint("Worker day count[" + (location.empty() ? "work" : location) + " " +
                                   std::to_string(lo) + ".." + std::to_string(hi) + " " +
                                   std::to_string(minCount) + ".." +
                                   (maxCount < 0 ? std::string("inf") : std::to_string(maxCount)) +
                                   "]",
                           weight, /*hard=*/false),
          lo_(lo), hi_(hi), min_(minCount), max_(maxCount), loc_(std::move(location)) {
        // hi - lo sizes the scratch table below. hi = -1 is the empty default range.
        if (lo < 0 || lo > MAX_DAY || hi < -1 || hi > MAX_DAY) {
            throw std::invalid_argument("worker_day_count_in_range: lo and hi must be days 0.." +
                                        std::to_string(MAX_DAY) + ", got " + std::to_string(lo) +
                                        ".." + std::to_string(hi));
        }
    }
    void onReset(const EmployeeSchedule& s) override {
        day_.resize(s.shifts.size());
        match_.resize(s.shifts.size());
        dayBit_.resize(s.shifts.size());
        fastMask_ = hi_ >= lo_ && hi_ - lo_ + 1 <= 64;
        for (std::size_t i = 0; i < s.shifts.size(); ++i) {
            day_[i] = static_cast<std::int32_t>(s.shifts[i].startDay);
            match_[i] = (loc_.empty() || s.shifts[i].location == loc_) ? 1 : 0;
            dayBit_[i] = fastMask_ && match_[i] && day_[i] >= lo_ && day_[i] <= hi_
                                 ? std::uint64_t{1} << static_cast<unsigned>(day_[i] - lo_)
                                 : 0;
        }
        seen_.assign(hi_ >= lo_ ? static_cast<std::size_t>(hi_ - lo_ + 1) : 0, 0);
    }
    double cost(const EmployeeSchedule& /*s*/, int /*unit*/,
                const std::vector<int>& members) const override {
        if (hi_ < lo_) return 0.0;
        int cnt = 0;
        if (fastMask_) {
            std::uint64_t seen = 0;
            for (int i : members) seen |= dayBit_[i];
            cnt = bitCount(seen);
        } else {
            for (int i : members) {
                const std::int32_t d = day_[i];
                if (match_[i] && d >= lo_ && d <= hi_ &&
                    !seen_[static_cast<std::size_t>(d - lo_)]) {
                    seen_[static_cast<std::size_t>(d - lo_)] = 1;
                    ++cnt;
                }
            }
            for (int i : members) {  // restore the scratch
                const std::int32_t d = day_[i];
                if (match_[i] && d >= lo_ && d <= hi_) {
                    seen_[static_cast<std::size_t>(d - lo_)] = 0;
                }
            }
        }
        double c = 0;
        if (cnt < min_) c += min_ - cnt;
        if (max_ >= 0 && cnt > max_) c += cnt - max_;
        return c;
    }

private:
    static int bitCount(std::uint64_t value) {
        int count = 0;
        while (value != 0) {
            value &= value - 1;
            ++count;
        }
        return count;
    }

    int lo_, hi_, min_, max_;
    std::string loc_;
    std::vector<std::int32_t> day_;
    std::vector<char> match_;
    std::vector<std::uint64_t> dayBit_;
    bool fastMask_ = false;
    mutable std::vector<char> seen_;  // scratch, zeroed after use
};

}  // namespace es

#endif  // ES_CONSTRAINTS_WORKER_DAY_COUNT_IN_RANGE_HPP

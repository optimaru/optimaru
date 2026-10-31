#ifndef ES_CONSTRAINTS_WORKER_PAIR_SAME_SHIFT_HPP
#define ES_CONSTRAINTS_WORKER_PAIR_SAME_SHIFT_HPP

#include "es/constraints/common.hpp"

namespace es {

// Soft: a directional worker pairing over a set of (day, location) target buckets in
// which BOTH workers are eligible (the caller resolves the candidate filter
// and passes only those buckets). For REWARD ("should share the same shift") a
// violation is a bucket where exactly one of the pair works it (XOR); for PENALTY
// ("should not share") a violation is a bucket where both work it. One violation
// per bucket. anchor/paired are engine worker
// indices. Only these two units affect the cost, so onUnitChanged ignores others.
struct WorkerPairSameShift final : EsGlobalConstraint {
    WorkerPairSameShift(double weight, int anchor, int paired,
                        std::vector<int> bucketDays, std::vector<std::string> bucketLocations,
                        bool penalty)
        : EsGlobalConstraint("Worker pairing[" + std::to_string(anchor) + (penalty ? "!=" : "==") +
                                     std::to_string(paired) + "]",
                             weight, /*hard=*/false),
          anchor_(anchor), paired_(paired), penalty_(penalty) {
        const std::size_t n = std::min(bucketDays.size(), bucketLocations.size());
        for (std::size_t i = 0; i < n; ++i)
            targets_.emplace(bucketDays[i], std::move(bucketLocations[i]));
    }
    void reset(const EmployeeSchedule& s,
               const std::vector<std::vector<int>>& membersPerUnit) override {
        day_.resize(s.shifts.size());
        loc_.resize(s.shifts.size());
        for (std::size_t i = 0; i < s.shifts.size(); ++i) {
            day_[i] = static_cast<int>(s.shifts[i].startDay);
            loc_[i] = s.shifts[i].location;
        }
        const int units = static_cast<int>(membersPerUnit.size());
        aMembers_ = (anchor_ >= 0 && anchor_ < units) ? membersPerUnit[anchor_] : std::vector<int>{};
        bMembers_ = (paired_ >= 0 && paired_ < units) ? membersPerUnit[paired_] : std::vector<int>{};
        recompute();
    }
    void onUnitChanged(const EmployeeSchedule&, int unit,
                       const std::vector<int>& members) override {
        if (unit == anchor_) {
            aMembers_ = members;
            recompute();
        } else if (unit == paired_) {
            bMembers_ = members;
            recompute();
        }
    }
    double cost() const override { return cost_; }

private:
    void recompute() {
        std::set<std::pair<int, std::string>> aWorks, bWorks;
        for (int i : aMembers_) aWorks.emplace(day_[i], loc_[i]);
        for (int i : bMembers_) bWorks.emplace(day_[i], loc_[i]);
        double c = 0;
        for (const auto& t : targets_) {
            const bool a = aWorks.count(t) > 0;
            const bool b = bWorks.count(t) > 0;
            if (penalty_ ? (a && b) : (a != b)) c += 1;
        }
        cost_ = c;
    }
    int anchor_, paired_;
    bool penalty_;
    std::set<std::pair<int, std::string>> targets_;  // (day, location) buckets both are eligible for
    std::vector<int> aMembers_, bMembers_;
    std::vector<int> day_;
    std::vector<std::string> loc_;
    double cost_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_WORKER_PAIR_SAME_SHIFT_HPP

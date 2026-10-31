#ifndef ES_CONSTRAINTS_SHIFT_COVERAGE_HPP
#define ES_CONSTRAINTS_SHIFT_COVERAGE_HPP

#include "es/constraints/common.hpp"

namespace es {

// Soft (default): daily staffing coverage. Groups shifts by (start day, shift
// location) and counts, per group, the assigned shifts whose employee has
// `workerSkill` (empty = count any assignee); penalizes each group below
// `minCount` (shortfall) or above `maxCount` (excess; maxCount < 0 = no upper
// bound). Models "at least K on this day+location" floors (set hard) and role-mix
// ranges like "between 2 and 4 holders of a skill per day+location" (soft). Global
// because coverage is a cross-employee count per group; maintained
// incrementally from stored per-unit members and per-group counts.
struct ShiftCoverage final : EsGlobalConstraint {
    // Optional rule scope (native, because coverage groups are derived from the
    // shift set): shiftIndices restrict which shifts form/require groups (out-of-
    // scope shifts are ignored — no phantom floor on their groups), workerIndices
    // restrict which workers count toward the floor. Empty => all.
    ShiftCoverage(double weight, std::string workerSkill, int minCount, int maxCount,
                  std::vector<int> workerIndices = {}, std::vector<int> shiftIndices = {})
        : EsGlobalConstraint(makeName(workerSkill, minCount, maxCount), weight, /*hard=*/false),
          skill_(std::move(workerSkill)),
          min_(minCount),
          max_(maxCount),
          workerIdx_(std::move(workerIndices)),
          shiftIdx_(std::move(shiftIndices)) {}

    void reset(const EmployeeSchedule& s,
               const std::vector<std::vector<int>>& membersPerUnit) override {
        std::vector<std::uint8_t> shiftIn(s.shifts.size(), shiftIdx_.empty() ? 1 : 0);
        for (int i : shiftIdx_)
            if (i >= 0 && i < static_cast<int>(s.shifts.size())) shiftIn[i] = 1;

        std::map<std::pair<DayIndex, std::string>, int> gmap;
        groupOf_.assign(s.shifts.size(), -1);
        for (std::size_t i = 0; i < s.shifts.size(); ++i) {
            if (!shiftIn[i]) continue;  // out-of-scope shift: no group, no floor
            std::pair<DayIndex, std::string> key(s.shifts[i].startDay, s.shifts[i].location);
            auto it = gmap.find(key);
            int g;
            if (it == gmap.end()) {
                g = static_cast<int>(gmap.size());
                gmap.emplace(key, g);
            } else {
                g = it->second;
            }
            groupOf_[i] = g;
        }
        nGroups_ = static_cast<int>(gmap.size());

        std::vector<std::uint8_t> workerIn(membersPerUnit.size(), workerIdx_.empty() ? 1 : 0);
        for (int u : workerIdx_)
            if (u >= 0 && u < static_cast<int>(membersPerUnit.size())) workerIn[u] = 1;
        hasSkill_.assign(membersPerUnit.size(), 0);
        for (std::size_t u = 0; u < s.employees.size(); ++u) {
            const bool skillOk = skill_.empty() || s.employees[u].skills.count(skill_);
            hasSkill_[u] = (skillOk && workerIn[u]) ? 1 : 0;
        }
        members_ = membersPerUnit;
        groupCount_.assign(nGroups_, 0);
        for (std::size_t u = 0; u < membersPerUnit.size(); ++u) {
            if (hasSkill_[u])
                for (int i : membersPerUnit[u])
                    if (groupOf_[i] >= 0) ++groupCount_[groupOf_[i]];
        }
        totalCost_ = 0;
        for (int count : groupCount_) totalCost_ += groupCost(count);
    }
    void onUnitChanged(const EmployeeSchedule&, int unit,
                       const std::vector<int>& members) override {
        if (hasSkill_[unit]) {
            for (int i : members_[unit])
                if (groupOf_[i] >= 0) changeGroup(groupOf_[i], -1);
            for (int i : members)
                if (groupOf_[i] >= 0) changeGroup(groupOf_[i], 1);
        }
        members_[unit] = members;
    }
    bool supportsMemberChange() const override { return true; }
    void onMemberChanged(const EmployeeSchedule&, int unit, const std::vector<int>&,
                         int entity, bool joined) override {
        if (hasSkill_[unit] && groupOf_[entity] >= 0) changeGroup(groupOf_[entity], joined ? 1 : -1);
    }
    double cost() const override { return totalCost_; }

private:
    static std::string makeName(const std::string& sk, int mn, int mx) {
        return "Shift coverage[" + (sk.empty() ? std::string("any") : sk) + " " +
               std::to_string(mn) + ".." + (mx < 0 ? std::string("inf") : std::to_string(mx)) +
               "]";
    }
    double groupCost(int count) const {
        double cost = 0;
        if (count < min_) cost += min_ - count;
        if (max_ >= 0 && count > max_) cost += count - max_;
        return cost;
    }
    void changeGroup(int group, int delta) {
        int& count = groupCount_[group];
        totalCost_ -= groupCost(count);
        count += delta;
        totalCost_ += groupCost(count);
    }
    std::string skill_;
    int min_, max_, nGroups_ = 0;
    std::vector<int> groupOf_;                 // shift -> group id
    std::vector<int> groupCount_;              // group -> count of matching assignments
    std::vector<char> hasSkill_;               // unit -> counts toward this coverage
    std::vector<std::vector<int>> members_;    // per-unit member snapshot (for diffs)
    std::vector<int> workerIdx_;               // scope: counting workers (empty = all)
    std::vector<int> shiftIdx_;                // scope: shifts forming groups (empty = all)
    double totalCost_ = 0;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_SHIFT_COVERAGE_HPP

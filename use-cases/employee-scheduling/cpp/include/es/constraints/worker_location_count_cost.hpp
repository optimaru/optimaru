#ifndef ES_CONSTRAINTS_WORKER_LOCATION_COUNT_COST_HPP
#define ES_CONSTRAINTS_WORKER_LOCATION_COUNT_COST_HPP

#include "es/constraints/common.hpp"

namespace es {

// SOFT, weight 1: workerLocationCountCost. Per employee holding
// `workerSkill` (empty = all), the count of its non-pinned shifts at
// `location` (empty = any) over the whole horizon, bounded to [minCount,
// maxCount] (maxCount < 0 = no upper bound). Raw cost per employee = the
// shortfall below minCount plus the excess above maxCount, so 0 = within
// bounds (the defaults 0..inf never cost anything). Example: ("Location B",
// "Skill B", 4, 8): holders of Skill B work 4 to 8 shifts at Location B over
// the horizon; ("Location B", "", 0, 10): nobody works more than 10 there.
// Each instance carries the label
// "Worker location count[<location|work> <skill|all> <min>..<max|inf>]".
struct WorkerLocationCountCost final : EsUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit WorkerLocationCountCost(double weight = kDefaultWeight, std::string location = {},
                                     std::string workerSkill = {}, int minCount = 0,
                                     int maxCount = -1)
        : EsUnitConstraint("workerLocationCountCost", weight, kDefaultLevel),
          loc_(std::move(location)), skill_(std::move(workerSkill)),
          min_(minCount), max_(maxCount) {
        label = "Worker location count[" + (loc_.empty() ? std::string("work") : loc_) + " " +
                (skill_.empty() ? std::string("all") : skill_) + " " + std::to_string(min_) + ".." +
                (max_ < 0 ? std::string("inf") : std::to_string(max_)) +
                "]";  // display only; never a lookup key
    }
    void on_reset(const EmployeeSchedule& s) override {
        match_.resize(s.shifts.size());
        for (std::size_t i = 0; i < s.shifts.size(); ++i)
            match_[i] = (loc_.empty() || s.shifts[i].location == loc_) ? 1 : 0;
        applies_.assign(s.employees.size(), skill_.empty() ? 1 : 0);
        if (!skill_.empty())
            for (std::size_t e = 0; e < s.employees.size(); ++e)
                applies_[e] = s.employees[e].skills.count(skill_) ? 1 : 0;
    }
    double cost(const EmployeeSchedule& s, int unit,
                const std::vector<int>& members) const override {
        if (!applies_[unit]) return 0.0;
        int cnt = 0;
        for (int i : members)
            if (!s.shifts[i].pinned) cnt += match_[i];  // the horizon count excludes pinned history
        double c = 0;
        if (cnt < min_) c += min_ - cnt;
        if (max_ >= 0 && cnt > max_) c += cnt - max_;
        return c;
    }

private:
    std::string loc_, skill_;
    int min_, max_;
    std::vector<char> match_, applies_;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_WORKER_LOCATION_COUNT_COST_HPP

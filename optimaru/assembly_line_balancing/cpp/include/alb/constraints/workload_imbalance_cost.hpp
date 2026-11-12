#ifndef ALB_CONSTRAINTS_WORKLOAD_IMBALANCE_COST_HPP
#define ALB_CONSTRAINTS_WORKLOAD_IMBALANCE_COST_HPP

#include <cstdint>
#include <set>
#include <vector>

#include "alb/constraints/common.hpp"

namespace alb {

// Soft, weight 0 (an opt-in objective, not scored by default): workloadImbalanceCost —
// how unevenly the used stations are loaded. Raw cost = max load - min load
// over the stations with a load > 0 (0 with no station in use); 0 = every
// used station carries the same processing time. One total (a global), so
// the exponent shapes the range.
//
// Per-station loads plus an ordered multiset of the non-zero ones, O(log
// stations) per change. Nothing but cost() reads them and reset() rebuilds
// them, so at weight 0 the objective sends this constraint no change
// (idle_at_weight_zero) and resets it when constraint_costs() reports the raw
// value for re-pricing.
struct WorkloadImbalanceCost final : MemberChangeGlobalConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit WorkloadImbalanceCost(double weight = kDefaultWeight)
        : MemberChangeGlobalConstraint("workloadImbalanceCost", weight, kDefaultLevel) {}

    void reset(const AssemblyLineBalancingProblem& problem,
               const std::vector<std::vector<int>>& members_per_unit) override {
        detail::requirePrepared(problem);
        load_.assign(members_per_unit.size(), 0);
        loads_.clear();
        for (std::size_t unit = 0; unit < members_per_unit.size(); ++unit) {
            for (int task : members_per_unit[unit]) load_[unit] += problem.tasks[task].duration;
            if (load_[unit] > 0) loads_.insert(load_[unit]);
        }
    }

    void on_member_changed(const AssemblyLineBalancingProblem& problem, int unit,
                           const std::vector<int>& /*members*/, int entity, bool joined) override {
        std::int64_t& load = load_[unit];
        if (load > 0) loads_.erase(loads_.find(load));
        load += joined ? problem.tasks[entity].duration : -problem.tasks[entity].duration;
        if (load > 0) loads_.insert(load);
    }

    double cost() const override {
        if (loads_.empty()) return 0.0;
        return static_cast<double>(*loads_.rbegin() - *loads_.begin());
    }

    bool idle_at_weight_zero() const override { return true; }

private:
    std::vector<std::int64_t> load_;     // per station
    std::multiset<std::int64_t> loads_;  // the non-zero station loads
};

}  // namespace alb

#endif  // ALB_CONSTRAINTS_WORKLOAD_IMBALANCE_COST_HPP

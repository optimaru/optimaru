#ifndef BP_CONSTRAINTS_ITEM_CONFLICT_COST_HPP
#define BP_CONSTRAINTS_ITEM_CONFLICT_COST_HPP

#include "bp/constraints/common.hpp"

namespace bp {

// Hard, weight 1: itemConflictCost — conflict pairs (must-separate items)
// placed in one bin. Raw cost per bin = the listed pairs whose two items are
// both in it; a pair listed twice (or reversed) counts per listing. 0 =
// every pair is apart (or has an unassigned item).
//
// Unit granularity: the cost is per BIN, so an exponent != 1 shapes the
// bin's count: two violated pairs in one bin at exponent 2 cost 4, not 1 + 1.
//
// Delta contract: a sum of pair terms, so the marginal cost of one member is
// the number of its partners (with multiplicity) among the other members —
// O(partners + members) with a scratch mark per item, 0 at once for an item
// without conflicts.
struct ItemConflictCost final : BpUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ItemConflictCost(double weight = kDefaultWeight)
        : BpUnitConstraint("itemConflictCost", weight, kDefaultLevel) {}
    void on_reset(const BinPackingProblem& problem) override {
        detail::requirePrepared(problem);
        mark_.assign(problem.items.size(), 0);
    }
    double cost(const BinPackingProblem& problem, int /*unit*/,
                const std::vector<int>& members) const override {
        // Every pair inside the bin is seen from both of its items.
        for (int item : members) mark_[item] = 1;
        int twice = 0;
        for (int item : members) {
            for (int partner : problem.conflictPartners[item]) twice += mark_[partner];
        }
        for (int item : members) mark_[item] = 0;
        return static_cast<double>(twice / 2);
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const BinPackingProblem& problem, int /*unit*/,
                      const std::vector<int>& members, int entity) const override {
        const std::vector<int>& partners = problem.conflictPartners[entity];
        if (partners.empty()) return 0.0;
        for (int partner : partners)
            ++mark_[partner];  // multiplicity: a pair listed twice counts 2
        int together = 0;
        for (int item : members) {
            if (item != entity) together += mark_[item];
        }
        for (int partner : partners) mark_[partner] = 0;
        return static_cast<double>(together);
    }

private:
    // Scratch: one counter per item, all 0 between calls.
    mutable std::vector<int> mark_;
};

}  // namespace bp

#endif  // BP_CONSTRAINTS_ITEM_CONFLICT_COST_HPP

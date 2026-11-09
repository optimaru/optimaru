#ifndef BP_CONSTRAINTS_COLOR_CONFLICT_COST_HPP
#define BP_CONSTRAINTS_COLOR_CONFLICT_COST_HPP

#include "bp/constraints/common.hpp"

namespace bp
{

// Hard, weight 1: colorConflictCost — items sharing a bin with another item
// of the same colour. Raw cost per bin = sum over colours c of
// max(0, n_c - 1), with n_c the bin's items of colour c, i.e. every item
// beyond the first of its colour; items without a colour never count. 0 =
// no bin holds two items of one colour.
//
// Unit granularity (0.2.0): the cost is per BIN. 0.1.5 listed one entry per
// present (bin, colour); the totals are identical for every assignment (and
// so is the score at exponent 1), but an exponent != 1 now shapes the bin's
// sum: red x3 + blue x2 in one bin at exponent 2 is 3^2 = 9, not 2^2 + 1^2.
//
// Delta contract: a member joining a bin changes max(0, n_c - 1) by exactly
// 1 when another member already has its colour, else by 0 — so delta_cost is
// that indicator, O(members) on the precomputed colour classes (O(1) for an
// item whose colour no other item has).
struct ColorConflictCost final : BpUnitConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit ColorConflictCost(double weight = kDefaultWeight)
        : BpUnitConstraint("colorConflictCost", weight, kDefaultLevel)
    {
    }
    void on_reset(const BinPackingProblem &problem) override
    {
        detail::requirePrepared(problem);
        seen_.assign(static_cast<std::size_t>(problem.colorClassCount), 0);
    }
    double cost(const BinPackingProblem &problem,
                int /*unit*/,
                const std::vector<int> &members) const override
    {
        // coloured members - distinct colours = sum of max(0, n_c - 1).
        int excess = 0;
        for (int item : members)
        {
            const int cls = problem.colorClass[item];
            if (cls < 0)
                continue;
            if (seen_[cls])
                ++excess;
            else
                seen_[cls] = 1;
        }
        for (int item : members)
        {
            const int cls = problem.colorClass[item];
            if (cls >= 0)
                seen_[cls] = 0;
        }
        return static_cast<double>(excess);
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const BinPackingProblem &problem,
                     int /*unit*/,
                     const std::vector<int> &members,
                     int entity) const override
    {
        const int cls = problem.colorClass[entity];
        if (cls < 0)
            return 0.0;
        for (int item : members)
        {
            if (item != entity && problem.colorClass[item] == cls)
                return 1.0;
        }
        return 0.0;
    }

private:
    // Scratch for cost(): one flag per colour class, all 0 between calls.
    mutable std::vector<char> seen_;
};

}  // namespace bp

#endif  // BP_CONSTRAINTS_COLOR_CONFLICT_COST_HPP

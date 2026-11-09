#ifndef BP_CONSTRAINTS_USED_BINS_COST_HPP
#define BP_CONSTRAINTS_USED_BINS_COST_HPP

#include "bp/constraints/common.hpp"

namespace bp
{

// Soft, weight 1: usedBinsCost — the objective, the number of bins in use.
// Raw cost = the bins holding at least one item (one global value; 0 = no
// item is assigned). 0.1.5 counted bins with a positive load, which is the
// same because every weight is > 0 (ScoreCalculator::prepare checks it).
//
// A GlobalConstraint on purpose: its exponent shapes the COUNT (count ** e,
// exactly as in 0.1.5, whose costs list was [count]); per-bin units would
// shape each bin's 1 instead. O(1) per move through the member-change fast
// path: a bin switches on when its first item joins and off when its last
// item leaves. A step function no single change can cross, which is why the
// solver ships EmptyBinMove and RepackMove (bp/moves.hpp).
struct UsedBinsCost final : BpGlobalConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit UsedBinsCost(double weight = kDefaultWeight)
        : BpGlobalConstraint("usedBinsCost", weight, kDefaultLevel)
    {
    }
    void reset(const BinPackingProblem &problem,
               const std::vector<std::vector<int>> &members_per_unit) override
    {
        detail::requirePrepared(problem);
        used_.assign(members_per_unit.size(), 0);
        count_ = 0;
        for (std::size_t bin = 0; bin < members_per_unit.size(); ++bin)
        {
            if (!members_per_unit[bin].empty())
            {
                used_[bin] = 1;
                ++count_;
            }
        }
    }
    void on_unit_changed(const BinPackingProblem & /*problem*/,
                       int unit,
                       const std::vector<int> &members) override
    {
        const char now = members.empty() ? 0 : 1;
        count_ += now - used_[unit];
        used_[unit] = now;
    }
    bool supports_member_change() const override { return true; }
    void on_member_changed(const BinPackingProblem &problem,
                         int unit,
                         const std::vector<int> &members,
                         int /*entity*/,
                         bool /*joined*/) override
    {
        on_unit_changed(problem, unit, members);  // `members` is after the change
    }
    double cost() const override { return static_cast<double>(count_); }

private:
    std::vector<char> used_;  // per bin: holds at least one item
    int count_ = 0;
};

}  // namespace bp

#endif  // BP_CONSTRAINTS_USED_BINS_COST_HPP

#ifndef ST_CONSTRAINTS_GLOBAL_BLOCK_ADJACENCY_HPP
#define ST_CONSTRAINTS_GLOBAL_BLOCK_ADJACENCY_HPP

#include "st/constraints/global/global_constraint.hpp"

namespace st::canon
{

// Hard (default): H8 — per block group with k assigned members: (k-1) minus
// the same-day consecutive-period adjacencies, plus one per missing member
// while 0 < k < groupSize (the shared blockViolation helper — see there for
// why a half-placed block is not free). The moved lesson's whole (small)
// group is recomputed per touch,
// from this component's own slot map (the solution's variable already holds
// the NEW value while the old unit's leave is delivered).
struct BlockAdjacency final : MemberChangeGlobalConstraint
{
    explicit BlockAdjacency(double weight = 1.0)
        : MemberChangeGlobalConstraint("H8 block adjacency",
                                       weight,
                                       /*hard=*/true)
    {
    }
    void reset(const Timetable &table,
               const std::vector<std::vector<int>> &membersPerUnit) override
    {
        detail::requirePrepared(table);
        slotOf_.assign(table.lessons.size(), -1);
        groupOf_.resize(table.lessons.size());
        for (std::size_t i = 0; i < table.lessons.size(); ++i)
            groupOf_[i] = table.lessons[i].blockGroup;
        for (std::size_t unit = 0; unit < membersPerUnit.size(); ++unit)
        {
            for (int entity : membersPerUnit[unit])
                slotOf_[entity] = static_cast<int>(unit);
        }
        violation_.assign(table.blockGroupCount, 0);
        total_ = 0;
        for (int group = 0; group < table.blockGroupCount; ++group)
            refresh(table, group);
    }
    void onMemberChanged(const Timetable &table,
                         int unit,
                         const std::vector<int> & /*members*/,
                         int entity,
                         bool joined) override
    {
        slotOf_[entity] = joined ? unit : -1;
        const int group = groupOf_[entity];
        if (group >= 0)
            refresh(table, group);
    }
    double cost() const override { return static_cast<double>(total_); }

private:
    void refresh(const Timetable &table, int group)
    {
        scratch_.clear();
        for (int i : table.blockMembers[group])
        {
            const int slot = slotOf_[i];
            if (slot >= 0)
            {
                scratch_.emplace_back(table.dayOfSlot[slot],
                                      table.periodOfSlot[slot]);
            }
        }
        // The missing-member charge lives in the shared helper, so this path,
        // the monolithic incremental calculator and computeBreakdown cannot
        // drift apart — canonical_plugin_test (a)/(b) gate exactly that.
        const std::int64_t violation = ScoreCalculator::blockViolation(
            scratch_, table.blockMembers[group].size());
        total_ += violation - violation_[group];
        violation_[group] = violation;
    }
    std::vector<int> slotOf_;              // lesson -> slot (-1 = unassigned)
    std::vector<std::int32_t> groupOf_;    // lesson -> block group (-1 = none)
    std::vector<std::int64_t> violation_;  // current H8 value per group
    std::vector<std::pair<int, int>> scratch_;
    std::int64_t total_ = 0;
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_GLOBAL_BLOCK_ADJACENCY_HPP

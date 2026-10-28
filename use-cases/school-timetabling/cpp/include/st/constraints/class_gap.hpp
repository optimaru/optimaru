#ifndef ST_CONSTRAINTS_CLASS_GAP_HPP
#define ST_CONSTRAINTS_CLASS_GAP_HPP

#include "st/constraints/global/global_constraint.hpp"

namespace st::canon
{

// Soft (default, weight = ObjectiveWeights::classGap): S4 — idle periods
// wedged inside a class's day, on the class-owned busy masks (one occupancy
// unit per lesson, so a merged lesson counts once).
struct ClassGap final : MemberChangeGlobalConstraint
{
    explicit ClassGap(double weight = ObjectiveWeights{}.classGap)
        : MemberChangeGlobalConstraint("S4 class gap", weight, /*hard=*/false)
    {
    }
    void reset(const Timetable &table,
               const std::vector<std::vector<int>> &membersPerUnit) override
    {
        detail::requirePrepared(table);
        masks_.reset(table.classCount, table.slotCount(), table.dayCount);
        // Dense projections (lesson -> class, slot -> day/period bit), so
        // the per-event path never strides the Lesson structs.
        const int lessonCount = static_cast<int>(table.lessons.size());
        classOf_.resize(lessonCount);
        for (int i = 0; i < lessonCount; ++i)
            classOf_[i] = table.lessons[i].classGroupId;
        const int slotCount = table.slotCount();
        slotDay_.resize(slotCount);
        slotBit_.resize(slotCount);
        for (int slot = 0; slot < slotCount; ++slot)
        {
            slotDay_[slot] = table.dayOfSlot[slot];
            slotBit_[slot] = detail::periodBit(table, slot);
        }
        total_ = 0;
        for (std::size_t unit = 0; unit < membersPerUnit.size(); ++unit)
        {
            for (int entity : membersPerUnit[unit])
                apply(static_cast<int>(unit), entity, /*joined=*/true);
        }
    }
    void onMemberChanged(const Timetable & /*table*/,
                         int unit,
                         const std::vector<int> & /*members*/,
                         int entity,
                         bool joined) override
    {
        apply(unit, entity, joined);
    }
    double cost() const override { return static_cast<double>(total_); }

private:
    void apply(int slot, int entity, bool joined)
    {
        masks_.apply(classOf_[entity],
                     slot,
                     slotDay_[slot],
                     slotBit_[slot],
                     joined,
                     [&](std::uint16_t oldMask, std::uint16_t newMask)
                     {
                         total_ += ScoreCalculator::gapOf(newMask)
                                   - ScoreCalculator::gapOf(oldMask);
                     });
    }
    detail::OwnerDayMasks masks_;
    std::vector<std::int32_t> classOf_;  // lesson -> class group
    std::vector<std::int32_t> slotDay_;
    std::vector<std::uint16_t> slotBit_;
    std::int64_t total_ = 0;
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_CLASS_GAP_HPP

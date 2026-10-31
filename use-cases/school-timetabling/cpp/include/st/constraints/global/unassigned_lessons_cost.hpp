#ifndef ST_CONSTRAINTS_GLOBAL_UNASSIGNED_LESSONS_COST_HPP
#define ST_CONSTRAINTS_GLOBAL_UNASSIGNED_LESSONS_COST_HPP

#include "st/constraints/global/global_constraint.hpp"

namespace st::canon
{

// unassignedLessonsCost — hard, weight 1. Raw cost = the number of lessons
// without a slot (slot == -1), so 0 = every weekly period of every course is
// placed. Global because an unassigned lesson belongs to no unit (the es
// UnassignedShiftCost pattern).
//
// Demoted to soft it turns the search into partial timetabling (the CP-SAT
// reference's relaxed weekly-period counts): the solver then adds unassign /
// re-assign moves, and the weight is the price of one unplaced period. Give
// it kSoftWeight then — well above the other soft terms, so every period
// that fits is placed; at the hard default 1 a lesson would be dropped to
// save a single teacher gap.
struct UnassignedLessonsCost final : MemberChangeGlobalConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;
    // The documented weight of this constraint demoted to soft (partial
    // timetabling, the server's allow_partial): what the CP-SAT reference and
    // the server always charged per unplaced period. Not applied by itself —
    // weight=None keeps kDefaultWeight on either level.
    static constexpr double kSoftWeight = 10000.0;

    explicit UnassignedLessonsCost(double weight = kDefaultWeight)
        : MemberChangeGlobalConstraint(
              "unassignedLessonsCost", weight, kDefaultLevel)
    {
    }
    void reset(const Timetable &table,
               const std::vector<std::vector<int>> &membersPerUnit) override
    {
        detail::requirePrepared(table);
        lessonCount_ = static_cast<std::int64_t>(table.lessons.size());
        assigned_ = 0;
        for (const std::vector<int> &members : membersPerUnit)
            assigned_ += static_cast<std::int64_t>(members.size());
    }
    void onMemberChanged(const Timetable & /*table*/,
                         int /*unit*/,
                         const std::vector<int> & /*members*/,
                         int /*entity*/,
                         bool joined) override
    {
        assigned_ += joined ? 1 : -1;
    }
    double cost() const override
    {
        return static_cast<double>(lessonCount_ - assigned_);
    }

private:
    std::int64_t lessonCount_ = 0;
    std::int64_t assigned_ = 0;
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_GLOBAL_UNASSIGNED_LESSONS_COST_HPP

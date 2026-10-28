#ifndef ST_CONSTRAINTS_GLOBAL_UNASSIGNED_LESSON_HPP
#define ST_CONSTRAINTS_GLOBAL_UNASSIGNED_LESSON_HPP

#include "st/constraints/global/global_constraint.hpp"

namespace st::canon
{

// Hard (default): H1 — one point per unassigned lesson. Global because an
// unassigned lesson belongs to no unit (the es UnassignedShift pattern).
struct UnassignedLesson final : MemberChangeGlobalConstraint
{
    explicit UnassignedLesson(double weight = 1.0)
        : MemberChangeGlobalConstraint("H1 unassigned lesson",
                                       weight,
                                       /*hard=*/true)
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

#endif  // ST_CONSTRAINTS_GLOBAL_UNASSIGNED_LESSON_HPP

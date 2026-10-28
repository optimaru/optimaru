#ifndef ST_CONSTRAINTS_GLOBAL_COURSE_DAILY_LIMIT_HPP
#define ST_CONSTRAINTS_GLOBAL_COURSE_DAILY_LIMIT_HPP

#include "st/constraints/global/global_constraint.hpp"

namespace st::canon
{

// Hard (default): H7 — per (courseKey, day): max(0, lessons - dailyLimit),
// maintained with the monolithic closed-form count deltas.
struct CourseDailyLimit final : MemberChangeGlobalConstraint
{
    explicit CourseDailyLimit(double weight = 1.0)
        : MemberChangeGlobalConstraint("H7 course daily limit",
                                       weight,
                                       /*hard=*/true)
    {
    }
    void reset(const Timetable &table,
               const std::vector<std::vector<int>> &membersPerUnit) override
    {
        detail::requirePrepared(table);
        dayCount_ = table.dayCount;
        // Dense projections, so the per-event path never strides the Lesson
        // structs: lesson -> course, slot -> day, course -> daily limit.
        const int lessonCount = static_cast<int>(table.lessons.size());
        courseOf_.resize(lessonCount);
        for (int i = 0; i < lessonCount; ++i)
            courseOf_[i] = table.lessons[i].courseKey;
        dayOfSlot_.assign(table.dayOfSlot.begin(), table.dayOfSlot.end());
        limit_.assign(table.courseDailyLimit.begin(),
                      table.courseDailyLimit.end());
        count_.assign(
            static_cast<std::size_t>(table.courseCount) * table.dayCount, 0);
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
    void apply(int unit, int entity, bool joined)
    {
        const int course = courseOf_[entity];
        const int day = dayOfSlot_[unit];
        int &count = count_[static_cast<std::size_t>(course) * dayCount_ + day];
        const int limit = limit_[course];
        const int next = count + (joined ? 1 : -1);
        total_ += ScoreCalculator::max0(next - limit)
                  - ScoreCalculator::max0(count - limit);
        count = next;
    }
    int dayCount_ = 0;
    std::vector<std::int32_t> courseOf_;   // lesson -> courseKey
    std::vector<std::int32_t> dayOfSlot_;  // slot -> day
    std::vector<std::int32_t> limit_;      // courseKey -> daily limit
    std::vector<int> count_;  // (courseKey, day) -> assigned lessons
    std::int64_t total_ = 0;
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_GLOBAL_COURSE_DAILY_LIMIT_HPP

#ifndef ST_CANONICAL_INCREMENTAL_CALCULATOR_HPP
#define ST_CANONICAL_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "st/canonical_domain.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace st::canon
{

// Constant-time membership list over a dense integer key space (the
// penalized-set trick): items() holds the current keys,
// pos_[key] the index into it or -1. Used to keep the hard-violation "hot"
// keys enumerable for the solver's violation-guided move selection.
class HotSet
{
public:
    void reset(std::size_t keyCount)
    {
        items_.clear();
        pos_.assign(keyCount, -1);
    }
    void set(int key, bool on)
    {
        const int p = pos_[key];
        if (on && p < 0)
        {
            pos_[key] = static_cast<int>(items_.size());
            items_.push_back(key);
        }
        else if (!on && p >= 0)
        {
            const int last = items_.back();
            items_[p] = last;
            pos_[last] = p;
            items_.pop_back();
            pos_[key] = -1;
        }
    }
    bool contains(int key) const { return pos_[key] >= 0; }
    std::size_t size() const { return items_.size(); }
    const std::vector<int> &items() const { return items_; }

private:
    std::vector<int> items_;
    std::vector<int> pos_;
};

// The canonical domain's optimaru::IncrementalScoreCalculator.
//
// The core state is a 16-bit busy-period mask per (owner, day) — owner being
// a teacher or a class — derived from per-(owner, slot) occupancy counts (a
// mask bit flips only on a 0<->1 count transition, so team-teaching overlaps
// keep the DISTINCT-period semantics of validate.py). All mask metrics
// (H9 daily, H10 runs, S1/S4 gaps, S2 workdays, S3 free days) are recomputed
// for just the touched (owner, day) in O(periods <= 16) and applied as a
// subtract-old / add-new delta; the weekly H9 total is carried per teacher.
//
//   - H3/H4 conflict pairs, H6 room-demand excess and H7 course-day excess
//     are count-map increments with closed-form deltas.
//   - H8 recomputes the moved lesson's whole block group (k <= ~4 members)
//     against the shared ScoreCalculator::blockViolation helper.
//   - H1/H5/S5/S6 are per-lesson lookups.
//
// beforeVariableChanged retracts the lesson's contribution while `slot` still
// holds the OLD value; afterVariableChanged inserts the NEW value. Unassigned
// lessons (slot == -1) contribute only H1, so the construction heuristic can
// score partially initialized solutions. The full-recompute reference is
// ScoreCalculator::computeBreakdown (checked in the tests and under
// EnvironmentMode::FULL_ASSERT).
class CanonicalIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<Timetable>
{
public:
    void resetWorkingSolution(Timetable &workingSolution) override;
    void beforeVariableChanged(int lessonIndex) override;
    void afterVariableChanged(int lessonIndex) override;
    Score calculateScore() override;

    // Violation-guided selection (the es guided-moves pattern): a uniformly
    // random lesson among those currently involved in a hard violation,
    // weighted by the number of hot violation sites. Returns -1 when the
    // working solution is feasible (no hard violations). Each hard term keeps
    // its violating keys in a HotSet updated alongside the score deltas, and
    // the key is resolved to one of its lessons by a bounded scan of the
    // owner's static lesson list.
    int hotLesson(std::mt19937_64 &rng) const;

    // True while any hard-violation hot set is non-empty (i.e. the working
    // solution has a hard violation other than H1 unassigned).
    bool hasHotViolations() const
    {
        return hotNotAllowed_.size() != 0 || hotClassCells_.size() != 0
               || hotTeacherCells_.size() != 0 || hotRoomCells_.size() != 0
               || hotCourseDays_.size() != 0 || hotBlocks_.size() != 0
               || hotTeacherDays_.size() != 0 || hotTeachersWeek_.size() != 0;
    }

    // Debug/test hook: recompute the expected hot membership from the
    // occupancy counters and compare with the maintained HotSets.
    bool hotSetsConsistent() const;

private:
    Timetable *table_ = nullptr;
    int slotCount_ = 0;
    int dayCount_ = 0;
    std::vector<int> assign_;  // slot per lesson (-1 == unassigned)

    // Occupancy counters (same shapes as the full recompute's locals).
    std::vector<int> classSlotCnt_;
    std::vector<int> teacherSlotCnt_;
    std::vector<int> roomDemandCnt_;
    std::vector<int> courseDayCnt_;
    std::vector<std::uint16_t> teacherDayMask_;
    std::vector<std::uint16_t> classDayMask_;
    std::vector<int> teacherWeekCnt_;      // distinct busy periods per week
    std::vector<std::int64_t> blockViol_;  // current H8 value per group
    std::vector<std::pair<int, int>> blockScratch_;

    // Running totals (all counted positive; signs applied in calculateScore).
    std::int64_t unassigned_ = 0;           // H1
    std::int64_t classPairs_ = 0;           // H3
    std::int64_t teacherPairs_ = 0;         // H4
    std::int64_t notAllowed_ = 0;           // H5
    std::int64_t roomExcess_ = 0;           // H6
    std::int64_t courseExcess_ = 0;         // H7
    std::int64_t blockTotal_ = 0;           // H8
    std::int64_t teacherDailyExcess_ = 0;   // H9 (daily)
    std::int64_t teacherWeeklyExcess_ = 0;  // H9 (weekly)
    std::int64_t consecutiveExcess_ = 0;    // H10
    std::int64_t teacherGap_ = 0;           // S1
    std::int64_t teacherWorkday_ = 0;       // S2
    std::int64_t teacherFreeDay_ = 0;       // S3
    std::int64_t classGap_ = 0;             // S4
    std::int64_t avoided_ = 0;              // S5
    std::int64_t nonPreferred_ = 0;         // S6

    // Hard-violation hot keys (see hotLesson): encoded as owner * slotCount +
    // slot (cells), owner * dayCount + day (owner-days), or the owner id
    // itself. Maintained by insert/retract/setTeacherDayMask/refreshBlock.
    HotSet hotClassCells_;    // H3: classSlotCnt >= 2
    HotSet hotTeacherCells_;  // H4: teacherSlotCnt >= 2
    HotSet hotNotAllowed_;    // H5: lesson on a forbidden slot
    HotSet hotRoomCells_;     // H6: demand > capacity per (type, slot)
    HotSet hotCourseDays_;    // H7: count > dailyLimit per (course, day)
    HotSet hotBlocks_;        // H8: block group with violation > 0
    HotSet hotTeacherDays_;   // H9 daily / H10: excess on a (teacher, day)
    HotSet hotTeachersWeek_;  // H9 weekly: over maxPerWeek
    // Static owner -> lesson indices maps for resolving hot keys to lessons.
    std::vector<std::vector<int>> lessonsOfClass_;
    std::vector<std::vector<int>> lessonsOfTeacher_;
    std::vector<std::vector<int>> lessonsOfCourse_;
    std::vector<std::vector<int>> lessonsOfRoomType_;

    void insert(int lessonIndex, int slot);
    void retract(int lessonIndex, int slot);

    // Uniform pick among the assigned lessons of `owners` that satisfy
    // `match(lessonIndex)` (reservoir sampling, k = 1); -1 when none.
    template <typename Match>
    int pickLesson(const std::vector<int> &owners,
                   std::mt19937_64 &rng,
                   Match match) const
    {
        int picked = -1;
        std::uint64_t seen = 0;
        for (int i : owners)
        {
            if (assign_[i] < 0 || !match(i))
                continue;
            ++seen;
            if (seen == 1 || rng() % seen == 0)
                picked = i;
        }
        return picked;
    }

    // Replace the (teacher, day) mask and apply the metric deltas (H9 daily +
    // weekly, H10, S1, S2, S3).
    void setTeacherDayMask(int teacher, int day, std::uint16_t newMask);
    // Replace the (class, day) mask and apply the S4 delta.
    void setClassDayMask(int classGroup, int day, std::uint16_t newMask);
    // Recompute one block group's H8 value from assign_.
    void refreshBlock(int group);

    std::size_t slotOffset(int owner, int slot) const
    {
        return static_cast<std::size_t>(owner) * slotCount_ + slot;
    }
    std::size_t dayOffset(int owner, int day) const
    {
        return static_cast<std::size_t>(owner) * dayCount_ + day;
    }
};

}  // namespace st::canon

#endif  // ST_CANONICAL_INCREMENTAL_CALCULATOR_HPP

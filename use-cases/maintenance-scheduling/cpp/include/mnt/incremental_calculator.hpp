#ifndef MNT_INCREMENTAL_CALCULATOR_HPP
#define MNT_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <random>
#include <vector>

#include "mnt/domain.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace mnt
{

// The maintenance-scheduling implementation of the engine's
// optimaru::IncrementalScoreCalculator — the C++ analog of implementing
// Java's IncrementalScoreCalculator instead of a ConstraintProvider.
//
// Every constraint is a function of per-day occupancy counts or of the job's
// own dates, so a single variable change is O(span + |tags| * span) where
// span = the job's calendar length (endDate - startDate, <= ~2x duration):
//   - hard 1 (crewConflict): summed pairwise same-crew overlap in days
//     equals the C(c, 2) pair sum over (crew, day) occupancy cells because
//     job intervals are contiguous. Moving a job in/out of a cell with c
//     other members changes the pair count by exactly c.
//   - hard 2/3, soft 4/5 (min start / max end / before / after ideal):
//     unary terms of the job's own start/end — O(1) add/subtract.
//   - soft 6 (tagConflict): the same pair-count identity over (tag, day)
//     cells; a pair sharing k tags and overlapping d days is counted k*d
//     times, exactly Java's intersection.size() * overlap.
//
// beforeVariableChanged retracts the job's contribution while crewDate still
// holds the OLD value; afterVariableChanged inserts the NEW value.
// Unassigned jobs (crewDate == -1) contribute nothing, so the construction
// heuristic can score partially initialized solutions. The full-recompute
// reference is ScoreCalculator::calculateScore (checked in the tests and
// under EnvironmentMode::FULL_ASSERT).
class MaintenanceSchedulingIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<MaintenanceSchedule>
{
public:
    void resetWorkingSolution(MaintenanceSchedule &workingSolution) override;
    void beforeVariableChanged(int jobIndex) override;
    void afterVariableChanged(int jobIndex) override;
    Score calculateScore() override;

    // --- violation guidance (the school-timetabling hot-set pattern) ---
    // A "hot cell" is a (tag, day) pair currently occupied by two or more
    // jobs, i.e. one that contributes to the tagConflict term — at 1'000 per
    // tag-day it is the dominant soft cost on every non-trivial instance.
    // The set is maintained in O(1) inside insert/retract (a counter crossing
    // 2 in either direction) and lets the solver's entityRecommender seed
    // moves on jobs that are actually in conflict. Empty once tagConflict is
    // 0, so guidance switches itself off.
    bool hasHotTagCells() const { return !hotCells_.empty(); }
    // The whole set, in no particular order (the tests check it against a
    // brute-force recomputation).
    const std::vector<int> &hotTagCells() const { return hotCells_; }
    // Packed as tagId * dayCount + (day - dayBase); -1 when there is none.
    int hotTagCell(std::mt19937_64 &rng) const
    {
        if (hotCells_.empty())
            return -1;
        return hotCells_[rng() % hotCells_.size()];
    }

    // True when no job currently occupies `crewIndex` on any day of
    // [beginOffset, endOffset) (offsets relative to dayBase) — i.e. placing a
    // job there adds no crewConflict. Lets the solver's value recommender
    // propose crews the job actually fits on.
    bool crewFree(int crewIndex, int beginOffset, int endOffset) const
    {
        const int *row = crewDayCnt_.data()
                         + static_cast<std::size_t>(crewIndex) * dayCount_;
        for (int day = beginOffset; day < endOffset; ++day)
        {
            if (row[day] != 0)
                return false;
        }
        return true;
    }

private:
    MaintenanceSchedule *schedule_ = nullptr;
    int dayCount_ = 0;
    std::vector<int> assign_;  // crewDate per job (-1 == unassigned)

    // Per-day occupancy counters (same shapes as the full recompute's
    // locals).
    std::vector<int> crewDayCnt_;
    std::vector<int> tagDayCnt_;

    // Running totals (all counted positive; signs applied in
    // calculateScore).
    std::int64_t crewOverlap_ = 0;        // hard 1 (days)
    std::int64_t minStartViolation_ = 0;  // hard 2 (days)
    std::int64_t maxEndViolation_ = 0;    // hard 3 (days)
    std::int64_t beforeIdeal_ = 0;        // soft 4 (days)
    std::int64_t afterIdeal_ = 0;         // soft 5 (days)
    std::int64_t tagOverlap_ = 0;         // soft 6 (tag-days)

    // Hot (tag, day) cells as an indexed set: hotCells_ is the dense member
    // list, hotCellPos_ maps a cell to its slot in it (-1 = not hot), so both
    // add and remove are O(1) (swap with the back).
    std::vector<int> hotCells_;
    std::vector<int> hotCellPos_;

    void hotAdd(std::size_t cell)
    {
        hotCellPos_[cell] = static_cast<int>(hotCells_.size());
        hotCells_.push_back(static_cast<int>(cell));
    }
    void hotRemove(std::size_t cell)
    {
        const int pos = hotCellPos_[cell];
        const int moved = hotCells_.back();
        hotCells_[pos] = moved;
        hotCellPos_[moved] = pos;
        hotCells_.pop_back();
        hotCellPos_[cell] = -1;
    }

    void insert(int jobIndex, int crewDate);
    void retract(int jobIndex, int crewDate);

    std::size_t crewDayOffset(int crewIndex, int dayOffset) const
    {
        return static_cast<std::size_t>(crewIndex) * dayCount_ + dayOffset;
    }
    std::size_t tagDayOffset(int tagId, int dayOffset) const
    {
        return static_cast<std::size_t>(tagId) * dayCount_ + dayOffset;
    }
};

}  // namespace mnt

#endif  // MNT_INCREMENTAL_CALCULATOR_HPP

#ifndef BA_INCREMENTAL_CALCULATOR_HPP
#define BA_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "ba/domain.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace ba
{

// The bed-allocation implementation of the engine's
// optimaru::IncrementalScoreCalculator — the C++ analog of implementing
// Java's IncrementalScoreCalculator instead of a ConstraintProvider.
//
// A stay's contribution decomposes into
//   - a per-stay part (hard 2/3/5/6/7, medium 8, soft 9~12): a pure function
//     of (stay, value), recomputed in O(equipment) on insert/retract;
//   - pair parts (hard 1 sameBedInSameNight, hard 4
//     differentGenderInSameGenderRoomInSameNight): both are sums over the
//     nights two stays share, so they are maintained from per-day occupancy
//     counters instead of occupant lists:
//         hard 1 = 1000 * sum over (bed, night) of C(occupants, 2)
//         hard 4 = 1000 * sum over (SAME_GENDER room, night) of male * female
//     Inserting a stay adds 1000 * (the counter's value before the
//     increment) per night, which telescopes to exactly those sums, so a
//     move costs O(nights) instead of O(occupants of the bed/room) — the
//     occupant lists grew with the planning horizon and dominated the hot
//     path on long-horizon instances.
//
// beforeVariableChanged retracts the stay's contribution while `bed` still
// holds the OLD value; afterVariableChanged inserts the NEW value.
// Uninitialized stays (bed == -1) contribute nothing, so the construction
// heuristic can score partially initialized solutions; the "no bed" sentinel
// contributes only its medium penalty. The full-recompute reference is
// ScoreCalculator::calculateScore (checked in the tests and under
// EnvironmentMode::FULL_ASSERT).
class BedAllocationIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<BedPlan>
{
public:
    void resetWorkingSolution(BedPlan &workingSolution) override;
    void beforeVariableChanged(int stayIndex) override;
    void afterVariableChanged(int stayIndex) override;
    optimaru::HardSoftScore calculateScore() override;

private:
    BedPlan *plan_ = nullptr;
    std::vector<int> assign_;  // planning value per stay (-1 uninitialized)

    // Per-(stay, room) single-stay impact table, rebuilt by
    // resetWorkingSolution. ScoreCalculator::singleStayImpact is a pure
    // function of (stay, room-of-bed) over problem facts that never change
    // during a solve (equipment loops + branch chain), so the delta path
    // looks it up instead of re-deriving it on every insert/retract. Real
    // beds contribute hard/soft only; the "no bed" sentinel contributes
    // medium only (sentinelMedium_).
    int roomCount_ = 0;
    std::vector<std::int64_t> singleHard_;      // stays x rooms
    std::vector<std::int64_t> singleSoft_;      // stays x rooms
    std::vector<std::int64_t> sentinelMedium_;  // per stay

    // Per-night occupancy of the real beds (sentinel stays are in neither
    // table). Day indices are offset by dayOffset_ so only the planning
    // horizon is allocated.
    int dayOffset_ = 0;
    int dayCount_ = 0;
    std::vector<std::int32_t> bedDay_;   // bedCount * dayCount_
    std::vector<std::int32_t> roomDay_;  // (rooms * 2 + gender) * dayCount_,
                                         // SAME_GENDER rooms only

    // Running totals (positive penalty units; signs applied in
    // calculateScore).
    std::int64_t hardSingles_ = 0;
    std::int64_t hardBedPairs_ = 0;
    std::int64_t hardRoomPairs_ = 0;
    std::int64_t medium_ = 0;
    std::int64_t softSingles_ = 0;

    void insert(int stayIndex, int value);
    void retract(int stayIndex, int value);
};

}  // namespace ba

#endif  // BA_INCREMENTAL_CALCULATOR_HPP

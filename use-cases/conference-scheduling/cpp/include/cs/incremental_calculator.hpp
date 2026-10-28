#ifndef CS_INCREMENTAL_CALCULATOR_HPP
#define CS_INCREMENTAL_CALCULATOR_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cs/domain.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace cs
{

// The conference-scheduling implementation of the engine's
// optimaru::IncrementalScoreCalculator — the C++ analog of implementing
// Java's IncrementalScoreCalculator instead of a ConstraintProvider.
//
// Every constraint is either
//   - unary (the tag/availability/compatibility family): its contribution is
//     a pure function of (talk, timeslot, room). resetWorkingSolution
//     precompiles per-(talk, timeslot) and per-(talk, room) count tables, so
//     a change is O(1);
//   - pairwise (conflicts, diversity, prerequisites, pause, ...): summed
//     against AGGREGATES of the other assigned talks instead of talk by
//     talk. Every pairwise term is a sum over the other slot of
//     (slot-pair coefficient) x (count of matching talks in that slot), so
//     per-slot per-tag occupancy counters answer a whole move delta in
//     O(timeslots x tags-of-the-talk): tag-conflict terms read per-slot
//     per-tag counts (theme/sector/content/mutex, the theme x audience joint
//     table, per-level content counts for the flow constraint, per-level and
//     per-language counts for the diversity rewards), roomConflict reads a
//     (slot, room) occupancy grid, and popularTalks reads per-capacity-class
//     Fenwick trees over the favorite-count ranks (count + duration-sum
//     prefixes). The only per-talk sweeps left are the genuinely sparse
//     relations: talks sharing a speaker (speakerConflict, pause) and the
//     prerequisite adjacency, both walked over precompiled short lists. All
//     aggregate sums are integer, so the totals are bit-identical to the
//     former O(n) pair sweep;
//   - occupancy-shaped (crowdControl): per-timeslot risk>0 counters with the
//     slot total maintained under the counter change, O(1);
//   - grouped (speakerMakespan): per-speaker day counters; a change refreshes
//     only the changed talk's speakers, O(speakers x days).
//
// beforeVariableChanged retracts the talk's contribution while slotRoom
// still holds the OLD value; afterVariableChanged inserts the NEW value.
// Unassigned talks (slotRoom == -1) contribute nothing, so the construction
// heuristic can score partially initialized solutions. The full-recompute
// reference is ScoreCalculator::calculateScore (checked in the tests and
// under EnvironmentMode::FULL_ASSERT).
class ConferenceSchedulingIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<ConferenceSchedule>
{
public:
    void resetWorkingSolution(ConferenceSchedule &workingSolution) override;
    void beforeVariableChanged(int talkIndex) override;
    void afterVariableChanged(int talkIndex) override;
    Score calculateScore() override;

    // Violation-guided selection support (the school-timetabling
    // hot-lesson pattern, sampling-based): O(1) check whether the talk's
    // CURRENT assignment visibly carries hard units — type-incompatible
    // cell, required/prohibited tag violation, unavailable speaker or room,
    // or a shared (slot, room) cell. Sparse cross-cell families (speaker
    // conflict across rooms, prerequisites, pause, mutex, crowd) are not
    // checked: this is a selection bias, not a score term, and the O(1)
    // families dominate the hard mass on this domain. False when
    // unassigned.
    bool isHotTalk(int talkIndex) const;

private:
    ConferenceSchedule *schedule_ = nullptr;
    int slotCount_ = 0;
    int roomCount_ = 0;
    int talkCount_ = 0;
    int dayCount_ = 0;
    std::vector<int> assign_;  // slotRoom per talk (-1 == unassigned)

    // Unary count tables (weights applied at lookup time).
    std::vector<int> hardSlotCnt_;    // talk x slot: 4 hard ts-tag counts
    std::vector<int> softSlotCnt_;    // talk x slot: 4 soft ts-tag counts
    std::vector<int> unavailSpkCnt_;  // talk x slot: unavailable speakers
    std::vector<char> slotCompat_;    // talk x slot
    std::vector<int> hardRoomCnt_;    // talk x room: 4 hard room-tag counts
    std::vector<int> softRoomCnt_;    // talk x room: 4 soft room-tag counts
    std::vector<char> roomCompat_;    // talk x room

    // ------------------------------------------------------------------
    // Dense per-family tag indices (the shared interned pool remapped to
    // 0..familySize-1 so per-slot count tables stay small).
    // ------------------------------------------------------------------
    int themeDim_ = 1;    // T
    int sectorDim_ = 1;   // Sec
    int audDim_ = 1;      // A
    int contentDim_ = 1;  // C
    int mutexDim_ = 1;    // M
    int levelDim_ = 1;    // L (distinct audienceLevels, ascending)
    int langDim_ = 1;     // G
    std::vector<std::vector<int>> themeD_;    // per talk, dense theme ids
    std::vector<std::vector<int>> sectorD_;   // per talk
    std::vector<std::vector<int>> audD_;      // per talk
    std::vector<std::vector<int>> contentD_;  // per talk
    std::vector<std::vector<int>> mutexD_;    // per talk
    std::vector<int> lvlD_;                   // per talk, dense level index
    std::vector<int> langD_;                  // per talk, dense language

    // Sparse relations.
    std::vector<std::vector<int>> spkTalks_;   // speaker -> talks
    std::vector<std::vector<int>> prereqRev_;  // i -> talks that require i
    std::vector<std::uint64_t> seen_;          // epoch stamps, per talk
    std::uint64_t epoch_ = 0;

    // ------------------------------------------------------------------
    // Assignment aggregates (updated on every insert/retract).
    // ------------------------------------------------------------------
    std::vector<int> slotCnt_;        // talks per slot
    std::vector<int> cellCnt_;        // talks per (slot, room)
    std::vector<int> themeCnt_;       // slot x T
    std::vector<int> sectorCnt_;      // slot x Sec
    std::vector<int> audCnt_;         // slot x A
    std::vector<int> contentCnt_;     // slot x C
    std::vector<int> mutexCnt_;       // slot x M
    std::vector<int> themeAudCnt_;    // slot x T x A (joint occupancy)
    std::vector<int> themeCellCnt_;   // (slot, room) x T
    std::vector<int> contentLvlCnt_;  // slot x C x L
    std::vector<int> levelCnt_;       // slot x L
    std::vector<int> langCnt_;        // slot x G

    // popularTalks: per capacity class (distinct room capacities,
    // ascending), Fenwick trees over the dense favorite-count rank storing
    // talk counts and slot-duration sums.
    int capDim_ = 0;                         // K distinct capacities
    int favDim_ = 0;                         // F distinct favoriteCounts
    std::vector<int> capVal_;                // class -> capacity
    std::vector<int> capCls_;                // room -> class
    std::vector<int> favRank_;               // talk -> 1-based rank
    std::vector<int> favCntBit_;             // K x (F + 1)
    std::vector<std::int64_t> favDurBit_;    // K x (F + 1)
    std::vector<int> capTotalCnt_;           // K
    std::vector<std::int64_t> capTotalDur_;  // K

    // crowdControl occupancy: risk>0 talks per timeslot, with the weighted
    // unit total maintained under every counter change.
    std::vector<int> crowdCnt_;
    std::int64_t crowdUnits_ = 0;

    // speakerMakespan: assigned-talk counts per (speaker, dense day) and the
    // weighted unit total, refreshed per affected speaker.
    std::vector<int> speakerDayCnt_;
    std::int64_t makespanUnits_ = 0;

    // Signed running score contributions of the unary + pairwise families
    // (crowd/makespan assembled from their unit totals in calculateScore).
    std::int64_t hardBase_ = 0;
    double softBase_ = 0.0;

    void insert(int talkIndex, int slotRoom);
    void retract(int talkIndex, int slotRoom);
    // Total (hard, soft) pairwise contribution of talk i at (slot, room)
    // against every OTHER assigned talk, read from the aggregates (which
    // must not contain i at call time).
    void pairTotals(int talkIndex,
                    int slot,
                    int room,
                    std::int64_t &hardUnits,
                    std::int64_t &softUnits) const;
    // Adds (sign=+1) / removes (sign=-1) talk i at (slot, room) to/from the
    // aggregates.
    void applyAggregates(int talkIndex, int slot, int room, int sign);
    void addUnaryContribution(int talkIndex, int slot, int room, int sign);
    // crowdControl slot units under a counter change (f(k) = k >= 3 ?
    // k * slotDur : 0).
    void applyCrowdDelta(int slot, int newCount, int oldCount);
    std::int64_t makespanUnitsOf(int speaker) const;

    // Fenwick helpers over the flattened per-class trees.
    void favAdd(int capClass, int rank, int cnt, std::int64_t dur);
    // Sum of counts / durations with rank <= the given rank.
    void favPrefix(int capClass, int rank, int &cnt, std::int64_t &dur) const;
};

}  // namespace cs

#endif  // CS_INCREMENTAL_CALCULATOR_HPP

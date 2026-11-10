#ifndef FAB_EVALUATOR_HPP
#define FAB_EVALUATOR_HPP

#include <cstdint>
#include <vector>

#include "fab/decoder.hpp"
#include "fab/domain.hpp"
#include "fab/score_terms.hpp"

namespace fab
{

// ---------------------------------------------------------------------------
// ScheduleEvaluator — the incremental raw costs of a schedule
// ---------------------------------------------------------------------------
// The engine behind both incremental paths (the monolithic
// SemiconductorIncrementalScoreCalculator and the shared schedule view of the
// plugin assembly). Data mode: the raw costs of `assignments`, computed once
// per reset. Search mode (schedule.searching):
//
//   1. a decision-value change only records the row (markChanged);
//   2. totals() re-decodes lazily, once, from the earliest readyStep of the
//      changed rows (Decoder: the steps before it cannot change);
//   3. every row whose placement changed (machine, start, end or list
//      position) marks its row, lot, old and new machine, their groups, the
//      resources it uses and its pin dirty; only those units are re-costed,
//      through the components' own unit formulas, and the totals move by
//      the difference;
//   4. while every row holds its seed value and the seed was complete, the
//      totals are the seed's (the supplied assignments, never decoded).
//
// Equal to rawCostsOf(decode(values)) — or of the seed — by construction:
// the unit formulas are the same and a unit is re-costed whenever anything it
// reads changed. The tests check it on random move sequences.
class ScheduleEvaluator
{
public:
    void reset(const SemiconductorSchedule &schedule);
    // `row`'s decision value is changing (or has changed): before or after
    // the write, any number of times.
    void markChanged(int row);
    void markAllChanged();
    const RawTotals &totals();

    // Steps decoded since reset (diagnostics: the incremental saving).
    std::int64_t decodedSteps() const { return decodedSteps_; }

private:
    void applyPending();
    void redecode(int fromStep);
    void updatePlacement(int row);
    void markRowDirty(int row);
    void markMachineDirty(int machine);
    void recost();

    const SemiconductorSchedule *schedule_ = nullptr;
    bool dataMode_ = true;
    RawTotals dataTotals_{};
    RawTotals seedTotals_{};
    RawTotals totals_{};

    // decode state
    Decoder decoder_;
    std::vector<int> option_;
    std::vector<std::int64_t> rank_;
    std::vector<int> decodedValue_;  // the value option_/rank_ hold per row
    int decodeFrom_ = Decoder::kNever;
    std::vector<int> pending_;
    std::vector<char> isPending_;
    std::vector<char> matchesSeed_;
    int seedMismatches_ = 0;
    std::int64_t decodedSteps_ = 0;

    // scoring view of the decoded schedule
    std::vector<ScorePlacement> place_;
    std::vector<char> placed_;
    std::vector<std::vector<int>> machineRows_;
    std::vector<int> positionOnMachine_;
    std::vector<std::vector<int>> machinesOfGroup_;

    // unit caches (raw, per term)
    std::vector<std::int64_t> rowEligibility_, rowPreference_;
    struct LotCache
    {
        std::int64_t precedence = 0, maxQueue = 0, minQueue = 0, transport = 0;
        std::int64_t maxCycle = 0, tardiness = 0, cycle = 0;
        bool completed = false;
        Time completion = 0;
    };
    std::vector<LotCache> lot_;
    struct MachineCache
    {
        std::int64_t capacity = 0, batchCapacity = 0, familyConflict = 0, setup = 0;
        std::int64_t unavailability = 0, minBatch = 0, load = 0;
    };
    std::vector<MachineCache> machine_;
    std::vector<std::int64_t> resourceCapacity_, resourceUnavailability_;
    std::vector<std::int64_t> pin_;
    std::vector<std::int64_t> group_;

    // dirty units
    std::vector<char> dirtyRow_, dirtyLot_, dirtyMachine_, dirtyResource_, dirtyPin_, dirtyGroup_;
    std::vector<int> dirtyRows_, dirtyLots_, dirtyMachines_, dirtyResources_, dirtyPins_, dirtyGroups_;
    bool dirtyMakespan_ = false;

    // scratch
    std::vector<int> touched_;
    std::vector<char> touchedMark_;
    PlacementList scratch_;
};

}  // namespace fab

#endif  // FAB_EVALUATOR_HPP

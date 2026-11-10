#ifndef FAB_SCORE_TERMS_HPP
#define FAB_SCORE_TERMS_HPP

#include <algorithm>
#include <array>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

#include "fab/domain.hpp"

namespace fab
{

// ---------------------------------------------------------------------------
// The 20 constraints as score terms
// ---------------------------------------------------------------------------
// One entry per constraint, in list_constraints() order. Every component
// (fab/constraints/*_cost.hpp) owns its raw-cost formula as a static
// function over one unit (a row, a lot, a machine, a resource, a pin, a
// machine group); the monolithic calculators and the shared schedule view
// of the plugin path call those same functions, so a formula is written
// exactly once.
enum class Term : int
{
    MachineEligibilityCost,
    LotRoutePrecedenceCost,
    MachineCapacityCost,
    BatchCapacityCost,
    BatchFamilyConflictCost,
    MissingSetupTimeCost,
    MachineUnavailabilityCost,
    MaxQueueTimeCost,
    MinQueueTimeCost,
    MissingTransportTimeCost,
    AuxiliaryResourceCapacityCost,
    AuxiliaryResourceUnavailabilityCost,
    PinnedAssignmentsCost,
    MaxCycleTimeCost,
    MachinePreferenceCost,
    MinBatchSizeCost,
    MachineLoadImbalanceCost,
    WeightedTardinessCost,
    CycleTimeCost,
    MakespanCost,
};
constexpr int kTermCount = 20;
constexpr int kHardTermCount = 14;  // the first 14 terms are hard by default

constexpr std::size_t termIndex(Term term) { return static_cast<std::size_t>(term); }

// Raw (unweighted, unshaped) totals of every term; 0 = no violation.
using RawTotals = std::array<std::int64_t, kTermCount>;
// The per-item raw costs of every term, in the reference item order (what
// constraint_items() returns): one list per term.
using RawItems = std::array<std::vector<std::int64_t>, kTermCount>;

// How the min-batch-size rule groups the assignments of one batch machine:
// by (family, key), the key being the assignment's batch id when it has a
// non-empty one, else its start time. An id and a start never compare equal.
struct BatchKey
{
    bool isId = false;
    std::int64_t first = 0;   // interned id, or the start
    std::int64_t second = 0;  // 0, or (decoded schedules) the end

    bool operator<(const BatchKey &o) const
    {
        return std::tie(isId, first, second) < std::tie(o.isId, o.first, o.second);
    }
    bool operator==(const BatchKey &o) const
    {
        return isId == o.isId && first == o.first && second == o.second;
    }
};

// One assignment as the score terms read it.
struct ScorePlacement
{
    int row = -1;
    int machine = -1;
    Time start = 0;
    Time end = 0;
    // Position in the assignment list: the last tie-break of the machine
    // order (start, end, lot id) — a stable sort.
    std::int64_t seq = 0;
    BatchKey batchKey;
};

// The assignments of one machine in (start, end, lot id, list position)
// order; of one resource, one entry per listing, in list order; of one lot,
// one pointer per route step (nullptr = no assignment).
using PlacementList = std::vector<const ScorePlacement *>;

namespace terms
{

inline Time overlap(Time startA, Time endA, Time startB, Time endB)
{
    return std::max<Time>(0, checked::sub(std::min(endA, endB), std::max(startA, startB)));
}

// Appends one item and returns it (items may be null: totals only).
inline std::int64_t emit(std::vector<std::int64_t> *items, std::int64_t value)
{
    if (items != nullptr)
        items->push_back(value);
    return value;
}

// Time-integrated excess over `capacity`: sweep +1 at each start and -1 at
// each end; between consecutive event times, max(0, active - capacity) times
// the gap.
inline std::int64_t overloadArea(const PlacementList &placements, int capacity)
{
    std::vector<std::pair<Time, int>> events;
    events.reserve(placements.size() * 2);
    for (const ScorePlacement *p : placements)
    {
        events.emplace_back(p->start, 1);
        events.emplace_back(p->end, -1);
    }
    std::sort(events.begin(), events.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    std::int64_t excess = 0;
    std::int64_t active = 0;
    bool hasPrevious = false;
    Time previous = 0;
    for (std::size_t i = 0; i < events.size();)
    {
        const Time moment = events[i].first;
        if (hasPrevious)
            excess = checked::add(excess, checked::mul(std::max<std::int64_t>(0, active - capacity),
                                                       checked::sub(moment, previous)));
        for (; i < events.size() && events[i].first == moment; ++i)
            active += events[i].second;
        previous = moment;
        hasPrevious = true;
    }
    return excess;
}

inline int familyOf(const SemiconductorSchedule &schedule, const ScorePlacement *p)
{
    return schedule.operationOfRow(p->row).familyIndex;
}

// The machine order of the score terms: (start, end, lot id), stable on the
// list position.
inline void sortMachineRows(const SemiconductorSchedule &schedule, PlacementList &rows)
{
    std::sort(rows.begin(), rows.end(),
              [&](const ScorePlacement *a, const ScorePlacement *b)
              {
                  const int lotA = schedule.lotOfRow(a->row).idOrder;
                  const int lotB = schedule.lotOfRow(b->row).idOrder;
                  return std::tie(a->start, a->end, lotA, a->seq)
                         < std::tie(b->start, b->end, lotB, b->seq);
              });
}

}  // namespace terms

// Data mode: the raw costs of an assignment list, with the per-item lists when `items` is given.
RawTotals rawCostsOf(const SemiconductorSchedule &schedule,
                     const std::vector<Assignment> &assignments,
                     RawItems *items = nullptr);

}  // namespace fab

#endif  // FAB_SCORE_TERMS_HPP

#ifndef FAB_DOMAIN_HPP
#define FAB_DOMAIN_HPP

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "optimaru/solution_traits.hpp"
#include "optimaru/score.hpp"

namespace fab
{

// ---------------------------------------------------------------------------
// Semiconductor fab scheduling: re-entrant lots on alternative tools
// ---------------------------------------------------------------------------
// Lots follow a route of operations. Each operation runs on one of its
// eligible machines (each option has its own duration and preference cost),
// possibly together with other lots on a batch tool, needs auxiliary
// resources (operators, reticles, ...) while it runs, and is bounded by
// queue-time limits towards the next route step. Machines and resources have
// downtime; consecutive runs of different recipe families need a setup;
// moving between equipment groups needs transport time. Pins freeze an
// operation on its machine and time.
//
// Times, durations and costs are int64 (`Time`): the Pydantic contract uses
// unbounded ints and the capacity areas multiply excess by time.
using Time = std::int64_t;

// Checked int64 arithmetic for the times the decoder derives and the costs
// the score terms sum. 0.1.x computed exact Python integers; a result outside
// int64 throws std::overflow_error (OverflowError in Python) instead of
// wrapping.
namespace checked
{

[[noreturn]] inline void overflow()
{
    throw std::overflow_error("semiconductor scheduling: a time or cost does not fit in int64 "
                              "(scale the times or priorities down)");
}

inline std::int64_t add(std::int64_t a, std::int64_t b)
{
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    if (b > 0 ? a > kMax - b : a < kMin - b)
        overflow();
    return a + b;
}

inline std::int64_t sub(std::int64_t a, std::int64_t b)
{
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    if (b < 0 ? a > kMax + b : a < kMin + b)
        overflow();
    return a - b;
}

inline std::int64_t mul(std::int64_t a, std::int64_t b)
{
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    if (a == 0 || b == 0)
        return 0;
    const bool out = a > 0 ? (b > 0 ? a > kMax / b : b < kMin / a) : (b > 0 ? a < kMin / b : a < kMax / b);
    if (out)
        overflow();
    return a * b;
}

}  // namespace checked

// The engine's Score as is: there is no medium level.
//   hard = -(sum of the 14 hard constraints' raw costs, weight 1 each)
//   soft = -(machinePreferenceCost + minBatchSizeCost
//            + machineLoadImbalanceCost + 10 * weightedTardinessCost
//            + cycleTimeCost + makespanCost)
// The weights are the components' kDefaultWeight (fab/constraints/*.hpp).
using Score = optimaru::Score;

// [start, end), end > start.
struct TimeWindow
{
    Time start = 0;
    Time end = 0;
};

struct Machine
{
    std::string id;
    std::string group;  // equipment group, e.g. LITHO or ETCH
    int batchCapacity = 1;  // simultaneous compatible lots; 1 = a unary tool
    int minimumBatchSize = 1;  // preferred minimum lots in one batch run
    std::vector<TimeWindow> unavailable;  // maintenance / downtime

    // Filled by ScoreCalculator::prepare:
    int groupIndex = -1;
    std::vector<TimeWindow> unavailableByStart;  // stable sort by start

    bool isBatch() const { return batchCapacity > 1; }
};

// Reusable operator, reticle, fixture or transport resource.
struct AuxiliaryResource
{
    std::string id;
    int capacity = 1;
    std::vector<TimeWindow> unavailable;
};

struct MachineOption
{
    int machine = 0;  // index into SemiconductorSchedule::machines
    Time duration = 0;
    std::int64_t preferenceCost = 0;
};

struct Operation
{
    std::string id;
    std::string family;  // recipe family: batching and setup transitions
    std::vector<MachineOption> eligibleMachines;
    // Indices into auxiliaryResources. Duplicates are kept: an operation
    // listing a resource twice uses it twice (the 0.1.x semantics).
    std::vector<int> requiredResources;
    Time minQueueTime = 0;  // minimum wait after this step
    std::optional<Time> maxQueueTime;  // maximum wait after this step

    // Filled by ScoreCalculator::prepare:
    int familyIndex = -1;
    // machine -> the LAST eligible option on it (the scorer's last-wins
    // lookup of a machine among the options), as (machine, option index).
    std::vector<std::pair<int, int>> lastOptionOfMachine;

    // The last option on `machine`, or nullptr when it is not eligible.
    const MachineOption *lastOptionOn(int machine) const
    {
        for (const auto &[m, index] : lastOptionOfMachine)
        {
            if (m == machine)
                return &eligibleMachines[static_cast<std::size_t>(index)];
        }
        return nullptr;
    }
};

struct Lot
{
    std::string id;
    std::string product;
    Time releaseTime = 0;
    Time dueTime = 0;
    std::int64_t priority = 1;
    std::optional<Time> maxCycleTime;
    std::vector<Operation> operations;  // the route, >= 1 step

    // Filled by ScoreCalculator::prepare:
    int firstRow = 0;  // row of operations[0]
    int idOrder = 0;   // rank of `id` among the lot ids (byte order)
};

struct SetupTime
{
    std::string machineGroup;
    std::string fromFamily;
    std::string toFamily;
    Time duration = 0;
};

struct TransportTime
{
    std::string fromGroup;
    std::string toGroup;
    Time duration = 0;
};

// One operation placed on a machine: lot / operation / machine indices.
struct Assignment
{
    int lot = 0;
    int operation = 0;
    int machine = 0;
    Time start = 0;
    Time end = 0;
    std::optional<std::string> batchId;
};

// ---------------------------------------------------------------------------
// OperationRow  (@PlanningEntity)
// ---------------------------------------------------------------------------
// One row per (lot, operation), lot order then route order. The planning
// variable folds the two decisions of the 0.1.x search state into one int:
//
//   value = rank * K + slot     (K = maxOptionCount, the most eligible
//                                machines of any operation)
//
//   rank in [0, rows)  the decode priority (ties fall back to the lot's
//                      priority, then its id)
//   slot in [0, K)     the machine option, used modulo the row's own option
//                      count (option index = slot % options)
//   -1                 unassigned: the decoder stops the lot here (this row
//                      and the rest of its route get no assignment)
//
// The schedule itself is derived: the serial schedule-generation decoder
// (fab/decoder.hpp) turns the values into assignments. Pinned rows are NOT
// engine-pinned: their rank still decides when the lot's next step becomes
// ready (their placement is the pin).
struct OperationRow
{
    int lot = 0;
    int operation = 0;
    int value = -1;
};

// ---------------------------------------------------------------------------
// SemiconductorSchedule  (@PlanningSolution)
// ---------------------------------------------------------------------------
// Two modes, one flag:
//   data mode (searching == false, the default): the score is that of
//     `assignments` exactly as given (calculate_score / constraint_costs);
//   search mode (searching == true, only inside Solver::solve and the tests
//     that drive a Editor): the score is that of the decoded planning
//     values — or of `seedAssignments` as supplied while every row holds its
//     seedValues and the seed was complete.
struct SemiconductorSchedule
{
    // --- facts ---
    std::vector<Machine> machines;
    std::vector<AuxiliaryResource> auxiliaryResources;
    std::vector<SetupTime> setupTimes;
    std::vector<TransportTime> transportTimes;
    std::vector<Lot> lots;
    std::vector<Assignment> pinnedAssignments;  // one per operation at most

    // --- input / output ---
    std::vector<Assignment> assignments;
    // Start the search from `assignments` (see Solver::solve); reset to false
    // once a solve has used it.
    bool warmStart = false;
    Time makespan = 0;
    std::int64_t totalWeightedTardiness = 0;
    std::int64_t totalCycleTime = 0;
    Score score;
    bool scored = false;  // false: makespan / totals / score are not set

    // --- search state (Solver::solve) ---
    std::vector<OperationRow> rows;
    bool searching = false;
    bool seedComplete = false;
    std::vector<int> seedValues;
    std::vector<Assignment> seedAssignments;

    // --- prepared by ScoreCalculator::prepare (idempotent) ---
    bool prepared = false;
    int maxOptionCount = 1;  // K
    std::vector<std::string> groupNames;   // machine groups, first appearance
    std::vector<std::string> familyNames;  // operation families, first appearance
    std::vector<int> pinOfRow;             // index into pinnedAssignments, -1
    std::vector<std::vector<int>> rowsUsingResource;  // with multiplicity

    int rowCount() const { return static_cast<int>(rows.size()); }
    int rowOf(int lot, int operation) const
    {
        return lots[static_cast<std::size_t>(lot)].firstRow + operation;
    }
    const Lot &lotOfRow(int row) const
    {
        return lots[static_cast<std::size_t>(rows[static_cast<std::size_t>(row)].lot)];
    }
    const Operation &operationOfRow(int row) const
    {
        const OperationRow &r = rows[static_cast<std::size_t>(row)];
        return lots[static_cast<std::size_t>(r.lot)]
            .operations[static_cast<std::size_t>(r.operation)];
    }
    int optionCountOfRow(int row) const
    {
        return static_cast<int>(operationOfRow(row).eligibleMachines.size());
    }

    // --- the composite value encoding (see OperationRow) ---
    int value_count() const { return rowCount() * maxOptionCount; }
    int valueOf(int rank, int slot) const { return rank * maxOptionCount + slot; }
    int rankOf(int value) const { return value / maxOptionCount; }
    int slotOf(int value) const { return value % maxOptionCount; }
    // The option index a value selects on `row`; -1 for an unassigned value.
    int optionIndexOf(int row, int value) const
    {
        return value < 0 ? -1 : slotOf(value) % optionCountOfRow(row);
    }

    // --- prepared lookups ---
    // Setup time between two families on a machine group: the last
    // setupTimes entry for the triple wins, a missing entry is 0.
    Time setupTime(int group, int fromFamily, int toFamily) const
    {
        if (group < 0 || fromFamily < 0 || toFamily < 0)
            return 0;
        const std::size_t f = familyNames.size();
        const std::size_t key = (static_cast<std::size_t>(group) * f
                                 + static_cast<std::size_t>(fromFamily))
                                    * f
                                + static_cast<std::size_t>(toFamily);
        const auto it = setupByKey.find(key);
        return it == setupByKey.end() ? 0 : it->second;
    }
    // Transport time between machine groups: last entry wins, missing = 0.
    Time transportTime(int fromGroup, int toGroup) const
    {
        return transportByGroup[static_cast<std::size_t>(fromGroup)
                                    * groupNames.size()
                                + static_cast<std::size_t>(toGroup)];
    }

    std::unordered_map<std::size_t, Time> setupByKey;
    std::vector<Time> transportByGroup;  // [from * groups + to]
};

}  // namespace fab

// ---------------------------------------------------------------------------
// SolutionTraits: entities are the operation rows, the planning variable
// is OperationRow::value, the value range is the (rank x slot) grid. No
// engine pinning (see OperationRow). value_count reads maxOptionCount, so
// ScoreCalculator::prepare must have run (Solver::solve and every calculator
// call it).
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionTraits<fab::SemiconductorSchedule>
{
    static constexpr optimaru::Shape shape = optimaru::Shape::Assignment;
    static int entity_count(const fab::SemiconductorSchedule &s)
    {
        return s.rowCount();
    }
    static int value_count(const fab::SemiconductorSchedule &s)
    {
        return s.value_count();
    }
    static int value_of(const fab::SemiconductorSchedule &s, int row)
    {
        return s.rows[static_cast<std::size_t>(row)].value;
    }
    static void assign(fab::SemiconductorSchedule &s, int row, int value)
    {
        s.rows[static_cast<std::size_t>(row)].value = value;
    }
};

#endif  // FAB_DOMAIN_HPP

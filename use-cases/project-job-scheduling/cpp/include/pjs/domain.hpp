#ifndef PJS_DOMAIN_HPP
#define PJS_DOMAIN_HPP

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace pjs
{

// ---------------------------------------------------------------------------
// Score  (HardMediumSoftScore mirror)
// ---------------------------------------------------------------------------
// The Java quickstart scores with HardMediumSoftScore; the generic engine
// (include/optimaru/) only knows HardSoftScore(int64 hard, double soft). Mapping rule
// (the bed-allocation / meeting-scheduling precedent):
//
//   engine.hard = hard
//   engine.soft = medium * kMediumUnit + soft      (kMediumUnit = 10^9)
//
// All project-job-scheduling constraints penalize whole days, so medium
// (total project delay) and soft (total makespan) are integers. The engine's
// double is exact up to 2^53, so the mapping preserves the HardMediumSoft
// lexicographic order as long as |medium| < ~9.0 * 10^6 and |soft| < 10^9 —
// both are bounded by allocationCount * (maxDelay + maxDuration), far below
// those limits for any realistic instance.
constexpr std::int64_t kMediumUnit = 1000000000LL;

struct Score
{
    std::int64_t hard = 0;
    std::int64_t medium = 0;
    std::int64_t soft = 0;

    bool isFeasible() const { return hard >= 0; }

    bool operator<(const Score &o) const
    {
        if (hard != o.hard)
            return hard < o.hard;
        if (medium != o.medium)
            return medium < o.medium;
        return soft < o.soft;
    }
    bool operator>(const Score &o) const { return o < *this; }
    bool operator<=(const Score &o) const { return !(o < *this); }
    bool operator>=(const Score &o) const { return !(*this < o); }
    bool operator==(const Score &o) const
    {
        return hard == o.hard && medium == o.medium && soft == o.soft;
    }

    std::string toString() const
    {
        return std::to_string(hard) + "hard/" + std::to_string(medium)
               + "medium/" + std::to_string(soft) + "soft";
    }
};

inline optimaru::HardSoftScore toEngineScore(const Score &s)
{
    return {s.hard,
            static_cast<double>(s.medium) * static_cast<double>(kMediumUnit)
                + static_cast<double>(s.soft)};
}

// Inverse of toEngineScore. Every constraint penalizes (medium/soft <= 0 and
// integral), so truncating integer division decodes the two levels exactly.
inline Score fromEngineScore(const optimaru::HardSoftScore &e)
{
    const std::int64_t combined
        = static_cast<std::int64_t>(std::llround(e.soft));
    return {e.hard, combined / kMediumUnit, combined % kMediumUnit};
}

// ---------------------------------------------------------------------------
// JobType  (org.acme.projectjobschedule.domain.JobType)
// ---------------------------------------------------------------------------
// Every project has one SOURCE job (the start milestone) and one SINK job
// (the end milestone); both carry a single zero-duration execution mode and
// their allocations are always pinned (Java's Allocation.setPinned).
enum class JobType
{
    SOURCE,
    STANDARD,
    SINK
};

// ---------------------------------------------------------------------------
// Project  (problem fact)
// ---------------------------------------------------------------------------
// Dates are integer day indices. releaseDate only feeds the "total project
// delay" constraint through criticalPathEndDate(); like the Java quickstart
// it does NOT constrain the schedule's start dates.
struct Project
{
    std::string id;
    int releaseDate = 0;
    int criticalPathDuration = 0;

    int criticalPathEndDate() const
    {
        return releaseDate + criticalPathDuration;
    }
};

// ---------------------------------------------------------------------------
// Resource  (problem fact)
// ---------------------------------------------------------------------------
// Java models GlobalResource (always renewable) and LocalResource (per
// project, renewable flag) as subclasses; here one struct carries both:
// project < 0 == global. Only `capacity` and `renewable` feed the
// constraints; `project` is informational (a local resource is simply never
// referenced by another project's execution modes).
//
//   renewable      capacity applies per day (e.g. people, machines)
//   non-renewable  capacity applies over the whole horizon (e.g. a budget)
struct Resource
{
    std::string id;
    int capacity = 0;
    bool renewable = true;
    int project = -1;  // index into ProjectJobSchedule::projects, -1 = global
};

// ---------------------------------------------------------------------------
// ResourceRequirement  (problem fact, nested in its ExecutionMode)
// ---------------------------------------------------------------------------
struct ResourceRequirement
{
    int resource = 0;  // index into ProjectJobSchedule::resources
    int requirement = 0;
};

// ---------------------------------------------------------------------------
// ExecutionMode  (problem fact, nested in its Job)
// ---------------------------------------------------------------------------
struct ExecutionMode
{
    std::string id;
    int duration = 0;  // in days
    std::vector<ResourceRequirement> resourceRequirements;
};

// ---------------------------------------------------------------------------
// Job  (problem fact)
// ---------------------------------------------------------------------------
// Every job needs at least one execution mode (Java's SOURCE/SINK carry one
// zero-duration mode). Precedence is stored on the allocations
// (predecessorAllocations), not here — the Java Job.successorJobs list is a
// data-loading convenience that DemoDataGenerator folds into the allocation
// graph, and this port keeps only the folded form.
struct Job
{
    std::string id;
    int project = 0;  // index into ProjectJobSchedule::projects
    JobType jobType = JobType::STANDARD;
    std::vector<ExecutionMode> executionModes;
};

// ---------------------------------------------------------------------------
// Allocation  (@PlanningEntity)
// ---------------------------------------------------------------------------
// The Java entity has TWO basic planning variables — executionMode (value
// range = the job's own executionModes) and delay (value range = [0, 500)) —
// plus a shadow chain predecessorsDoneDate -> startDate -> endDate. The C++
// engine supports one BASIC int variable per entity with one global value
// range, so both variables are composed into a single value:
//
//   modeDelay = delay * maxModeCount + modeSlot,   modeDelay in
//               [0, delayRangeSize * maxModeCount)
//
// where maxModeCount = max over all jobs of executionModes.size() (computed
// by prepare()). Jobs with fewer modes than maxModeCount decode the slot
// modulo their own mode count (executionModeIndexOfValue), so every value in
// the global range is valid for every entity — the encoding is redundant for
// those jobs but never produces an out-of-range mode, exactly preserving the
// Java per-entity value range semantics. -1 means unassigned (both Java
// variables null).
//
// The Java shadow variables are ported as derived state: the score
// calculators recompute predecessorsDoneDate/startDate/endDate in topological
// order (the incremental calculator re-propagates only the successor
// subgraph of a changed allocation). Like the Java suppliers, an unassigned
// allocation passes dates through with delay 0 / duration 0 and contributes
// no resource usage. The fields below hold the last full recompute
// (ScoreCalculator::calculateScore fills them; Solver::solve leaves them
// consistent with the final assignment).
struct Allocation
{
    std::string id;
    int job = 0;  // index into ProjectJobSchedule::jobs
    // Precedence graph over allocations (indices into
    // ProjectJobSchedule::allocations). Successor lists are derived by
    // prepare(). Must be acyclic.
    std::vector<int> predecessorAllocations;

    int modeDelay = -1;  // composite planning variable; -1 == unassigned

    // Pinned allocations keep their modeDelay fixed. SOURCE/SINK allocations
    // are ALWAYS pinned regardless of this flag (Java's setPinned); the flag
    // additionally pins STANDARD allocations.
    bool pinned = false;

    // Derived state (the Java shadow variables); see the class comment.
    int predecessorsDoneDate = 0;
    int startDate = 0;
    int endDate = 0;
};

// ---------------------------------------------------------------------------
// ProjectJobSchedule  (@PlanningSolution)
// ---------------------------------------------------------------------------
struct ProjectJobSchedule
{
    std::vector<Project> projects;
    std::vector<Resource> resources;
    std::vector<Job> jobs;
    std::vector<Allocation> allocations;

    // Java's Allocation.getDelayRange() is IntStream.range(0, 500): delays
    // 0..delayRangeSize-1.
    int delayRangeSize = 500;

    Score score;

    // Set once by ScoreCalculator::prepare (idempotent); validates the
    // instance and fills the derived fields below.
    bool prepared = false;

    // Filled by ScoreCalculator::prepare:
    int maxModeCount = 1;
    std::vector<int> topologicalOrder;  // allocation indices, sources first
    std::vector<std::vector<int>> successorAllocations;  // inverse of preds

    // --- composite modeDelay encoding helpers (see Allocation) ---
    int allocationCount() const { return static_cast<int>(allocations.size()); }
    int valueCount() const { return delayRangeSize * maxModeCount; }
    int executionModeCountOf(int allocationIndex) const
    {
        return static_cast<int>(
            jobs[allocations[allocationIndex].job].executionModes.size());
    }
    int valueOf(int delay, int modeSlot) const
    {
        return delay * maxModeCount + modeSlot;
    }
    int delayOfValue(int value) const { return value / maxModeCount; }
    int modeSlotOfValue(int value) const { return value % maxModeCount; }
    // The Java executionMode planning variable: slot modulo the job's own
    // mode count, so every global value decodes to a valid mode.
    int executionModeIndexOfValue(int allocationIndex, int value) const
    {
        return modeSlotOfValue(value) % executionModeCountOf(allocationIndex);
    }
    const ExecutionMode &executionModeOf(int allocationIndex, int value) const
    {
        const Job &job = jobs[allocations[allocationIndex].job];
        return job
            .executionModes[executionModeIndexOfValue(allocationIndex, value)];
    }
    JobType jobTypeOf(int allocationIndex) const
    {
        return jobs[allocations[allocationIndex].job].jobType;
    }
    // Java's Allocation.getProjectCriticalPathEndDate().
    int projectCriticalPathEndDateOf(int allocationIndex) const
    {
        return projects[jobs[allocations[allocationIndex].job].project]
            .criticalPathEndDate();
    }
};

}  // namespace pjs

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization  (the @PlanningEntity/@PlanningVariable
// annotations of the C++ engine): entities are allocations, the planning
// variable is Allocation::modeDelay, the value range is the (delay x
// executionMode) grid. isPinned mirrors Java's @PlanningPin + setPinned:
// SOURCE/SINK allocations are always pinned, STANDARD ones when the user
// flags them. valueCount reads maxModeCount, so ScoreCalculator::prepare must
// have run (Solver::solve and both calculators call it).
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionDescriptor<pjs::ProjectJobSchedule>
{
    static constexpr optimaru::VariableKind kind
        = optimaru::VariableKind::BASIC;
    static int entityCount(const pjs::ProjectJobSchedule &s)
    {
        return s.allocationCount();
    }
    static int valueCount(const pjs::ProjectJobSchedule &s)
    {
        return s.valueCount();
    }
    static int getVariable(const pjs::ProjectJobSchedule &s,
                           int allocationIndex)
    {
        return s.allocations[allocationIndex].modeDelay;
    }
    static void
    setVariable(pjs::ProjectJobSchedule &s, int allocationIndex, int value)
    {
        s.allocations[allocationIndex].modeDelay = value;
    }
    static bool isPinned(const pjs::ProjectJobSchedule &s, int allocationIndex)
    {
        return s.jobTypeOf(allocationIndex) != pjs::JobType::STANDARD
               || s.allocations[allocationIndex].pinned;
    }
};

#endif  // PJS_DOMAIN_HPP

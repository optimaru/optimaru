#ifndef PJS_DOMAIN_HPP
#define PJS_DOMAIN_HPP

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/score/levels.hpp"

namespace pjs {

// ---------------------------------------------------------------------------
// Score  (hard / medium / soft)
// ---------------------------------------------------------------------------
// The domain scores on hard / medium / soft levels; the generic engine
// (optimaru/cpp/optimaru/) only knows Score(int64 hard, double soft). Mapping rule
// (the bed-allocation / meeting-scheduling precedent):
//
//   engine.hard = hard
//   engine.soft = medium * kMediumUnit + soft      (kMediumUnit = 10^9)
//
// All project-job-scheduling constraints penalize whole days, so medium
// (projectDelayCost, the summed project delay) and soft (makespanCost) are
// integers. The engine's double is exact up to 2^53, so the mapping
// preserves the hard / medium / soft lexicographic order as long as
// |medium| < ~9.0 * 10^6 and |soft| < 10^9 — both are bounded by
// allocationCount * (maxDelay + maxDuration), far below those limits for any
// realistic instance.
//
// The fold is declared ONCE, here: toEngineScore below packs the monolithic
// Score with it, and the MediumFoldOf<ProjectJobSchedule> specialization at
// the end of this header hands the same constant to the decomposed
// calculator, which keeps a separate medium accumulator. The medium
// constraint's weight is therefore in medium units (projectDelayCost: 1),
// never 10^9.
constexpr std::int64_t kMediumUnit = 1000000000LL;
constexpr optimaru::MediumFold kMediumFold =
        optimaru::MediumFold::into_soft(static_cast<double>(kMediumUnit));

struct Score {
    std::int64_t hard = 0;
    std::int64_t medium = 0;
    std::int64_t soft = 0;

    bool feasible() const { return hard >= 0; }

    bool operator<(const Score& o) const {
        if (hard != o.hard) return hard < o.hard;
        if (medium != o.medium) return medium < o.medium;
        return soft < o.soft;
    }
    bool operator>(const Score& o) const { return o < *this; }
    bool operator<=(const Score& o) const { return !(o < *this); }
    bool operator>=(const Score& o) const { return !(*this < o); }
    bool operator==(const Score& o) const {
        return hard == o.hard && medium == o.medium && soft == o.soft;
    }

    std::string str() const {
        return std::to_string(hard) + "hard/" + std::to_string(medium) + "medium/" +
               std::to_string(soft) + "soft";
    }
};

inline optimaru::Score toEngineScore(const Score& s) {
    // soft + medium * 10^9: for the integral levels of this domain the same
    // value as the old `medium * kMediumUnit + soft` (exact while
    // |engine soft| < 2^53, see above).
    return kMediumFold.to_engine(s.hard, static_cast<double>(s.medium),
                                 static_cast<double>(s.soft));
}

// A best-score LIMIT (Solver::Config::target_score) in engine terms.
// kNoLevelFloor (INT64_MIN) on the medium or the soft level means "no floor
// on the soft side": the engine's soft limit becomes -inf, so only the hard
// level is compared and no soft score, however low, can miss the stop.
// stop_at_first_feasible passes (0, kNoLevelFloor, kNoLevelFloor). Through
// toEngineScore the sentinel would fold into a finite engine soft that a
// low enough soft score never reaches. Any other limit folds as a score.
constexpr std::int64_t kNoLevelFloor = std::numeric_limits<std::int64_t>::min();

inline optimaru::Score toEngineScoreLimit(const Score& limit) {
    if (limit.medium == kNoLevelFloor || limit.soft == kNoLevelFloor)
        return {limit.hard, -std::numeric_limits<double>::infinity()};
    return toEngineScore(limit);
}

// Inverse of toEngineScore. Every constraint penalizes (medium/soft <= 0 and
// integral), so truncating integer division decodes the two levels exactly.
inline Score fromEngineScore(const optimaru::Score& e) {
    const std::int64_t combined = optimaru::round_score_level(e.soft, "soft");
    return {e.hard, combined / kMediumUnit, combined % kMediumUnit};
}

// ---------------------------------------------------------------------------
// JobType
// ---------------------------------------------------------------------------
// Every project has one SOURCE job (the start milestone) and one SINK job
// (the end milestone); both carry a single zero-duration execution mode and
// their allocations are always pinned.
enum class JobType { SOURCE, STANDARD, SINK };

// ---------------------------------------------------------------------------
// Project  (problem fact)
// ---------------------------------------------------------------------------
// Dates are integer day indices. releaseDate only feeds the projectDelayCost
// constraint through criticalPathEndDate(); it does NOT constrain the
// schedule's start dates.
struct Project {
    std::string id;
    int releaseDate = 0;
    int criticalPathDuration = 0;

    int criticalPathEndDate() const { return releaseDate + criticalPathDuration; }
};

// ---------------------------------------------------------------------------
// Resource  (problem fact)
// ---------------------------------------------------------------------------
// One struct carries both global resources (always renewable) and local
// ones (per project, renewable flag): project < 0 == global. Only `capacity` and `renewable` feed
// the constraints; `project` is informational (a local resource is simply never referenced by
// another project's execution modes).
//
//   renewable      capacity applies per day (e.g. people, machines)
//   non-renewable  capacity applies over the whole horizon (e.g. a budget)
struct Resource {
    std::string id;
    int capacity = 0;
    bool renewable = true;
    int project = -1;  // index into ProjectJobSchedule::projects, -1 = global
};

// ---------------------------------------------------------------------------
// ResourceRequirement  (problem fact, nested in its ExecutionMode)
// ---------------------------------------------------------------------------
struct ResourceRequirement {
    int resource = 0;  // index into ProjectJobSchedule::resources
    int requirement = 0;
};

// ---------------------------------------------------------------------------
// ExecutionMode  (problem fact, nested in its Job)
// ---------------------------------------------------------------------------
struct ExecutionMode {
    std::string id;
    int duration = 0;  // in days
    std::vector<ResourceRequirement> resourceRequirements;
};

// ---------------------------------------------------------------------------
// Job  (problem fact)
// ---------------------------------------------------------------------------
// Every job needs at least one execution mode (SOURCE/SINK carry one
// zero-duration mode). Precedence is stored on the allocations
// (predecessorAllocations), not here.
struct Job {
    std::string id;
    int project = 0;  // index into ProjectJobSchedule::projects
    JobType jobType = JobType::STANDARD;
    std::vector<ExecutionMode> executionModes;
};

// ---------------------------------------------------------------------------
// Allocation  (the entity the solver assigns)
// ---------------------------------------------------------------------------
// An allocation has TWO decisions — executionMode (value range = the job's
// own executionModes) and delay (value range = [0, delayRangeSize)) — plus
// the derived dates predecessorsDoneDate -> startDate -> endDate. The
// engine supports one assignment int variable per entity with one global value
// range, so both decisions are composed into a single value:
//
//   modeDelay = delay * maxModeCount + modeSlot,   modeDelay in
//               [0, delayRangeSize * maxModeCount)
//
// where maxModeCount = max over all jobs of executionModes.size() (computed
// by prepare()). Jobs with fewer modes than maxModeCount decode the slot
// modulo their own mode count (executionModeIndexOfValue), so every value in
// the global range is valid for every entity — the encoding is redundant for
// those jobs but never produces an out-of-range mode. -1 means unassigned
// (neither a mode nor a delay).
//
// The dates are derived state: the score calculators recompute
// predecessorsDoneDate/startDate/endDate in topological order (the
// incremental calculator re-propagates only the successor subgraph of a
// changed allocation). An unassigned allocation passes dates through with delay 0 / duration 0 and
// contributes no resource usage. The fields below hold the last full recompute
// (ScoreCalculator::score fills them; Solver::solve leaves them
// consistent with the final assignment).
struct Allocation {
    std::string id;
    int job = 0;  // index into ProjectJobSchedule::jobs
    // Precedence graph over allocations (indices into
    // ProjectJobSchedule::allocations). Successor lists are derived by
    // prepare(). Must be acyclic.
    std::vector<int> predecessorAllocations;

    int modeDelay = -1;  // composite decision variable; -1 == unassigned

    // Pinned allocations keep their modeDelay fixed. SOURCE/SINK allocations
    // are ALWAYS pinned regardless of this flag; the flag additionally pins
    // STANDARD allocations.
    bool pinned = false;

    // Derived state; see the class comment.
    int predecessorsDoneDate = 0;
    int startDate = 0;
    int endDate = 0;
};

// ---------------------------------------------------------------------------
// ProjectJobSchedule  (the problem and its solution)
// ---------------------------------------------------------------------------
struct ProjectJobSchedule {
    std::vector<Project> projects;
    std::vector<Resource> resources;
    std::vector<Job> jobs;
    std::vector<Allocation> allocations;

    // The delay range: delays 0..delayRangeSize-1.
    int delayRangeSize = 500;

    Score score;

    // Set once by ScoreCalculator::prepare (idempotent); validates the
    // instance and fills the derived fields below.
    bool prepared = false;

    // Filled by ScoreCalculator::prepare:
    int maxModeCount = 1;
    std::vector<int> topologicalOrder;                   // allocation indices, sources first
    std::vector<std::vector<int>> successorAllocations;  // inverse of preds

    // --- composite modeDelay encoding helpers (see Allocation) ---
    int allocationCount() const { return static_cast<int>(allocations.size()); }
    int value_count() const { return delayRangeSize * maxModeCount; }
    int executionModeCountOf(int allocationIndex) const {
        return static_cast<int>(jobs[allocations[allocationIndex].job].executionModes.size());
    }
    int valueOf(int delay, int modeSlot) const { return delay * maxModeCount + modeSlot; }
    int delayOfValue(int value) const { return value / maxModeCount; }
    int modeSlotOfValue(int value) const { return value % maxModeCount; }
    // The execution mode a value selects: slot modulo the job's own
    // mode count, so every global value decodes to a valid mode.
    int executionModeIndexOfValue(int allocationIndex, int value) const {
        return modeSlotOfValue(value) % executionModeCountOf(allocationIndex);
    }
    const ExecutionMode& executionModeOf(int allocationIndex, int value) const {
        const Job& job = jobs[allocations[allocationIndex].job];
        return job.executionModes[executionModeIndexOfValue(allocationIndex, value)];
    }
    JobType jobTypeOf(int allocationIndex) const {
        return jobs[allocations[allocationIndex].job].jobType;
    }
    // The critical-path end date of the allocation's project.
    int projectCriticalPathEndDateOf(int allocationIndex) const {
        return projects[jobs[allocations[allocationIndex].job].project].criticalPathEndDate();
    }
};

}  // namespace pjs

// ---------------------------------------------------------------------------
// SolutionTraits specialization  (how the engine sees the domain): entities
// are allocations, the decision variable is Allocation::modeDelay, the value
// range is the (delay x executionMode) grid. is_pinned:
// SOURCE/SINK allocations are always pinned, STANDARD ones when the user
// flags them. value_count reads maxModeCount, so ScoreCalculator::prepare must
// have run (Solver::solve and both calculators call it).
// ---------------------------------------------------------------------------
template <>
struct optimaru::SolutionTraits<pjs::ProjectJobSchedule> {
    static constexpr optimaru::Shape shape = optimaru::Shape::Assignment;
    static int entity_count(const pjs::ProjectJobSchedule& s) { return s.allocationCount(); }
    static int value_count(const pjs::ProjectJobSchedule& s) { return s.value_count(); }
    static int value_of(const pjs::ProjectJobSchedule& s, int allocationIndex) {
        return s.allocations[allocationIndex].modeDelay;
    }
    static void assign(pjs::ProjectJobSchedule& s, int allocationIndex, int value) {
        s.allocations[allocationIndex].modeDelay = value;
    }
    static bool is_pinned(const pjs::ProjectJobSchedule& s, int allocationIndex) {
        return s.jobTypeOf(allocationIndex) != pjs::JobType::STANDARD ||
               s.allocations[allocationIndex].pinned;
    }
};

// ---------------------------------------------------------------------------
// Medium fold registration
// ---------------------------------------------------------------------------
// The decomposed calculator folds its medium accumulator with the domain's
// kMediumFold (see the Score adapter above).
template <>
struct optimaru::MediumFoldOf<pjs::ProjectJobSchedule> {
    static constexpr optimaru::MediumFold value = pjs::kMediumFold;
};

#endif  // PJS_DOMAIN_HPP

#ifndef FP_DOMAIN_HPP
#define FP_DOMAIN_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace fp
{

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
// The Java quickstart uses java.time.LocalDateTime and Duration; every
// constraint measures whole minutes (Duration.between(..).toMinutes()). We
// represent a timestamp as int64 minutes from an arbitrary common reference
// (the demo uses the schedule's Monday 00:00 as minute 0; the Python models
// use epoch minutes), so all arithmetic is exact integer math. kNoTime stands
// in for a null LocalDateTime (unassigned shadow values).
using Minutes = std::int64_t;

constexpr Minutes kNoTime = std::numeric_limits<std::int64_t>::min();

// ---------------------------------------------------------------------------
// Score  (HardMediumSoftScore mirror)
// ---------------------------------------------------------------------------
// The Java quickstart scores with HardMediumSoftScore; the generic engine
// (include/optimaru/) only knows HardSoftScore(int64 hard, double soft). Mapping rule
// (the bed-allocation port's convention, with a larger unit because the
// makespan penalty is quadratic):
//
//   engine.hard = hard
//   engine.soft = medium * kMediumUnit + soft      (kMediumUnit = 10^10)
//
// Every constraint penalizes whole minutes (or minutes squared), so medium
// and soft are integers and the combined int64 converts to double exactly
// while |medium * kMediumUnit + soft| < 2^53. That preserves the
// HardMediumSoft lexicographic order as long as
//   |soft|   < 10^10  (sum of per-line makespan^2; makespan up to ~10^5
//                      minutes == ~70 days per line), and
//   |medium| < 9*10^5 (total ideal-end lateness + unassigned duration,
//                      in minutes == ~625 days),
// which holds with big margin for quickstart-sized instances (demo: |soft|
// ~1.3*10^8, |medium| < 10^5). Feasibility stays hard-only, like Java.
constexpr std::int64_t kMediumUnit = 10000000000LL;  // 10^10

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
    return {s.hard, static_cast<double>(s.medium * kMediumUnit + s.soft)};
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
// Product  (problem fact)
// ---------------------------------------------------------------------------
// Java Product carries a Map<Product, Duration> cleaningDurations keyed by
// the PREVIOUS product on the line; here the whole map lives on the solution
// as a flat P x P matrix (see PackagingSchedule::cleanupMinutes).
struct Product
{
    std::string id;
    std::string name;
};

// Marks a (previous, next) product pair with no cleanup entry. Java throws
// IllegalArgumentException when such a pair is queried by the shadow chain
// (Product.getCleanupDuration); the port does the same on first use.
constexpr Minutes kMissingCleanup = -1;

// ---------------------------------------------------------------------------
// Operator  (@PlanningEntity in Java, only as the inverse of Line.operator)
// ---------------------------------------------------------------------------
struct Operator
{
    std::string id;
};

// ---------------------------------------------------------------------------
// Job  (@PlanningEntity — a LIST-variable element)
// ---------------------------------------------------------------------------
// The genuine assignment is the (line, position) implied by
// Line::jobs / PackagingSchedule::unassignedJobs. The Java shadow variables
//   line               (@InverseRelationShadowVariable)
//   lineOperator       (line.operator)
//   previousJob/nextJob (@Previous/@NextElementShadowVariable)
//   startCleaningDateTime / startProductionDateTime / endDateTime
// are pure functions of the list position and are derived here as
//   startCleaning(first job)  = line.startDateTime
//   startProduction(first)    = line.startDateTime
//   startCleaning(other)      = previous job's endDateTime
//   startProduction(other)    = startCleaning
//                               + cleanup(previous product -> this product)
//   endDateTime               = startProduction + duration
// (Note Java's quirk, kept verbatim: the FIRST job starts production at the
// line start with a zero-length cleaning window, and minStartTime is not
// consulted by the shadow chain nor by any constraint.)
// ScoreCalculator::updateShadowVariables materializes them onto the fields
// below; during a solve the incremental calculator maintains the same values
// internally per line (recomputed per touched list, O(list length)).
struct Job
{
    std::string id;
    std::string name;
    int productIndex = -1;
    Minutes durationMinutes = 0;
    Minutes minStartTime = 0;  // data only: no Java constraint reads it
    Minutes idealEndTime = 0;
    Minutes maxEndTime = 0;
    int priority = 1;  // data only: no Java constraint reads it (Java doc:
                       // higher number = higher priority)

    // @PlanningPin: a pinned job keeps its exact (line, position) — or stays
    // unassigned — and is scored like any other job. Because the engine's
    // LIST moves cannot skip elements, pinning is enforced structurally: the
    // SolutionDescriptor below exposes only unpinned jobs to the engine, and
    // pinned jobs must form a PREFIX of their line (the quickstart's
    // continuous-planning flow pins everything before "now", which is always
    // a prefix). ScoreCalculator::prepare validates this.
    bool pinned = false;

    // --- shadow variables (see the class comment) ---
    int lineIndex = -1;  // -1 == unassigned (Java: line == null)
    Minutes startCleaningDateTime = kNoTime;
    Minutes startProductionDateTime = kNoTime;
    Minutes endDateTime = kNoTime;
};

// ---------------------------------------------------------------------------
// Line  (@PlanningEntity)
// ---------------------------------------------------------------------------
// jobs is the @PlanningListVariable(allowsUnassignedValues = true): the
// ordered job sequence. operatorIndex mirrors the Java basic
// @PlanningVariable Line.operator; the generic engine drives ONE variable
// model per solution (here: the job lists), so the operator assignment is
// optimized by fp::Solver in an alternating outer loop instead of jointly
// (see solver.hpp for the exact semantics and the caveat).
struct Line
{
    std::string id;
    std::string name;
    Minutes startDateTime = 0;
    int operatorIndex = -1;  // -1 == unassigned (Java: operator == null)
    std::vector<int> jobs;   // ordered job indices
};

// ---------------------------------------------------------------------------
// PackagingSchedule  (@PlanningSolution)
// ---------------------------------------------------------------------------
// WorkCalendar is inlined as fromMinute/toMinute (informational, like Java:
// no constraint reads it). unassignedJobs is the port's stand-in for
// allowsUnassignedValues: the engine sees it as one extra list (the "pool"),
// so its moves can unassign a job by relocating it there. A job in no list
// at all is engine-uninitialized: the construction heuristic will place it
// (possibly into the pool, when leaving it unassigned scores best) — the
// same options as the reference model's construction heuristic.
struct PackagingSchedule
{
    Minutes fromMinute = 0;  // work calendar start (inclusive), midnight
    Minutes toMinute = 0;    // work calendar end (exclusive), midnight

    std::vector<Product> products;
    // cleanupMinutes[previousProduct * products.size() + product], filled
    // with kMissingCleanup where Java's map has no entry.
    std::vector<Minutes> cleanupMinutes;
    std::vector<Operator> operators;
    std::vector<Line> lines;
    std::vector<Job> jobs;
    std::vector<int> unassignedJobs;  // the pool pseudo-list (see above)

    Score score;

    // Set once by ScoreCalculator::prepare; guards against rebuilding the
    // engine view on every call.
    bool prepared = false;

    // --- engine view, filled by ScoreCalculator::prepare ---
    // The engine enumerates elements as [0, engineToJob.size()); pinned jobs
    // are excluded, and each list is exposed minus its pinned prefix.
    std::vector<int> engineToJob;      // engine element id -> job index
    std::vector<int> jobToEngine;      // job index -> element id (-1 pinned)
    std::vector<int> pinnedPrefixLen;  // per list: lines then the pool

    int lineCount() const { return static_cast<int>(lines.size()); }
    int poolListIndex() const { return lineCount(); }
    int listCountWithPool() const { return lineCount() + 1; }

    const std::vector<int> &rawList(int listIndex) const
    {
        return listIndex < lineCount() ? lines[listIndex].jobs : unassignedJobs;
    }
    std::vector<int> &rawList(int listIndex)
    {
        return listIndex < lineCount() ? lines[listIndex].jobs : unassignedJobs;
    }

    // Cleanup duration when `product` follows `previousProduct` on a line;
    // kMissingCleanup when Java's map has no entry (the caller throws).
    Minutes cleanup(int previousProduct, int product) const
    {
        return cleanupMinutes[static_cast<std::size_t>(previousProduct)
                                  * products.size()
                              + static_cast<std::size_t>(product)];
    }
};

}  // namespace fp

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization (the @PlanningEntity/@PlanningListVariable
// annotations of the C++ engine): the lists are the packaging lines plus the
// unassigned pool, the elements are the unpinned jobs. Position/element ids
// are translated so pinned jobs (always a validated prefix of their list)
// are invisible to the engine — they can never be moved, swapped, ruined or
// shifted, which reproduces Java's @PlanningPin for list elements.
// ScoreCalculator::prepare must run before the engine touches the solution.
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionDescriptor<fp::PackagingSchedule>
{
    static constexpr optimaru::VariableKind kind = optimaru::VariableKind::LIST;

    static int listCount(const fp::PackagingSchedule &s)
    {
        return s.listCountWithPool();
    }
    static int elementCount(const fp::PackagingSchedule &s)
    {
        return static_cast<int>(s.engineToJob.size());
    }
    static int listSize(const fp::PackagingSchedule &s, int l)
    {
        return static_cast<int>(s.rawList(l).size()) - s.pinnedPrefixLen[l];
    }
    static int getElement(const fp::PackagingSchedule &s, int l, int p)
    {
        return s.jobToEngine[s.rawList(l)[s.pinnedPrefixLen[l] + p]];
    }
    static void
    insertElement(fp::PackagingSchedule &s, int l, int p, int element)
    {
        std::vector<int> &list = s.rawList(l);
        list.insert(list.begin() + s.pinnedPrefixLen[l] + p,
                    s.engineToJob[element]);
    }
    static int removeElement(fp::PackagingSchedule &s, int l, int p)
    {
        std::vector<int> &list = s.rawList(l);
        const int job = list[s.pinnedPrefixLen[l] + p];
        list.erase(list.begin() + s.pinnedPrefixLen[l] + p);
        return s.jobToEngine[job];
    }
    static void
    reverseSegment(fp::PackagingSchedule &s, int l, int first, int last)
    {
        std::vector<int> &list = s.rawList(l);
        std::reverse(list.begin() + s.pinnedPrefixLen[l] + first,
                     list.begin() + s.pinnedPrefixLen[l] + last + 1);
    }
};

#endif  // FP_DOMAIN_HPP

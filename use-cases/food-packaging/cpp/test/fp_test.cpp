// Food-packaging unit tests (engine test/ style: check() + non-zero exit on any
// failure):
//   (a) every constraint verified against hand-computed full scores (the
//       Java FoodPackagingConstraintProviderTest cases re-derived through
//       the real shadow chain — the Java ConstraintVerifier presets shadow
//       values and isolates one constraint; a full-score calculator cannot,
//       so every expectation here accounts for all five constraints)
//   (b) incremental score == full recompute across a long random walk of
//       LIST moves driven through the engine's ScoreDirector protocol,
//       with shared operators, a pinned line prefix and a pinned
//       unassigned job in play
//   (c) a small instance solved under EnvironmentMode::FULL_ASSERT
//       (including the LNS ruin/recreate path)
//   (d) pinned jobs keep their exact position / stay unassigned through a
//       solve; the unassigned pool works as the allowsUnassignedValues
//       stand-in
//   (e) operator-assignment helpers (Java's Line.operator variable, handled
//       by the wrapper's alternating loop)
//   (f) the Java demo instance (Random(37) replica) has the expected shape
//       and reaches 0hard
//   (g) HardMediumSoft <-> engine HardSoft mapping roundtrip + input
//       validation errors
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "fp/demo_data.hpp"
#include "fp/domain.hpp"
#include "fp/incremental_calculator.hpp"
#include "fp/score_calculator.hpp"
#include "fp/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace fp;

namespace
{

int failures = 0;

void check(bool ok, const std::string &label)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok)
        ++failures;
}

void checkScore(const std::string &label,
                const Score &actual,
                const Score &expected)
{
    const bool ok = actual == expected;
    std::printf("  [%s] %-46s got=%s expected=%s\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual.toString().c_str(),
                expected.toString().c_str());
    if (!ok)
        ++failures;
}

// A time far in the future: maxEnd/idealEnd constraints never fire.
constexpr Minutes BIG = 1000000000LL;

// The Java test's products: 0 = A small, 1 = A large, 2 = B, with the same
// cleaning-duration maps plus 0-minute self entries (the demo generator also
// emits self = 0) so same-product sequences work through the chain.
void addTestProducts(PackagingSchedule &s)
{
    s.products = {
        {"1", "Product A small"}, {"2", "Product A large"}, {"3", "Product B"}};
    // cleanupMinutes[previous * 3 + current]
    s.cleanupMinutes = {
        0,
        0,
        40,  // previous: A small
        5,
        0,
        40,  // previous: A large
        60,
        60,
        0,  // previous: B
    };
}

constexpr int PA_S = 0;  // product index 1 ("Product A large") is unused by
constexpr int PB = 2;    // the cases below but keeps the Java test's matrix

struct JobSpec
{
    int product;
    Minutes duration;
    Minutes idealEnd = BIG;
    Minutes maxEnd = BIG;
    bool pinned = false;
};

int addJob(PackagingSchedule &s, const JobSpec &spec)
{
    Job job;
    job.id = std::to_string(s.jobs.size());
    job.name = "job" + job.id;
    job.productIndex = spec.product;
    job.durationMinutes = spec.duration;
    job.minStartTime = 0;
    job.idealEndTime = spec.idealEnd;
    job.maxEndTime = spec.maxEnd;
    job.pinned = spec.pinned;
    s.jobs.push_back(std::move(job));
    return static_cast<int>(s.jobs.size()) - 1;
}

void addLine(PackagingSchedule &s,
             int operatorIndex,
             const std::vector<JobSpec> &specs)
{
    Line line;
    line.id = std::to_string(s.lines.size());
    line.name = "Line " + line.id;
    line.startDateTime = 0;
    line.operatorIndex = operatorIndex;
    for (const JobSpec &spec : specs)
        line.jobs.push_back(addJob(s, spec));
    s.lines.push_back(std::move(line));
}

PackagingSchedule baseSchedule(int operatorCount)
{
    PackagingSchedule s;
    s.fromMinute = 0;
    s.toMinute = 14 * 24 * 60;
    addTestProducts(s);
    for (int o = 0; o < operatorCount; ++o)
        s.operators.push_back({"Operator " + std::to_string(o + 1)});
    return s;
}

// -------------------------------------------------------------------------
// (a) Hand-computed full scores through the real shadow chain.
// -------------------------------------------------------------------------
void verifyConstraints()
{
    std::printf("== (a) Verifying each constraint (hand-computed) ==\n");

    {
        // maxEndDateTime. Chain: J0 [clean 0,0) run 0..200; J1 clean [200,
        // 200) (A small -> A small = 0), run 200..350. J1 is 100 late
        // against maxEnd 250; makespan 350^2.
        PackagingSchedule s = baseSchedule(1);
        addLine(s, 0, {{PA_S, 200, BIG, 200}, {PA_S, 150, BIG, 250}});
        checkScore("maxEndDateTime 100 minutes late",
                   ScoreCalculator::calculateScore(s),
                   {-100, 0, -350 * 350});
    }
    {
        // The cleanup chain and the shadow values themselves. J1 cleans
        // from J0's end (100) for cleanup(B -> A small) = 60, so production
        // runs 160..260.
        PackagingSchedule s = baseSchedule(1);
        addLine(s, 0, {{PB, 100}, {PA_S, 100}});
        checkScore("cleanup chain (B -> A small = 60)",
                   ScoreCalculator::calculateScore(s),
                   {0, 0, -260 * 260});
        const Job &j1 = s.jobs[1];
        check(j1.lineIndex == 0 && j1.startCleaningDateTime == 100
                  && j1.startProductionDateTime == 160 && j1.endDateTime == 260,
              "shadow chain values (startCleaning/startProduction/end)");
        const Job &j0 = s.jobs[0];
        check(j0.startCleaningDateTime == 0 && j0.startProductionDateTime == 0
                  && j0.endDateTime == 100,
              "first job: zero-length cleaning window at line start");
    }
    {
        // operatorCleaningConflict: lines 0 and 1 share operator 0. Cleaning
        // windows: line0 job1 [10, 70), line1 job1 [30, 90) -> overlap 40.
        // Line 2 (operator 1) has window [5, 65) but no same-operator
        // partner. Makespans: 170^2 + 190^2 + 165^2.
        PackagingSchedule s = baseSchedule(2);
        addLine(s, 0, {{PB, 10}, {PA_S, 100}});
        addLine(s, 0, {{PB, 30}, {PA_S, 100}});
        addLine(s, 1, {{PB, 5}, {PA_S, 100}});
        checkScore("operatorCleaningConflict 40 overlap minutes",
                   ScoreCalculator::calculateScore(s),
                   {-40, 0, -(170 * 170 + 190 * 190 + 165 * 165)});
    }
    {
        // idealEndDateTime: same chain as the maxEnd case but on the medium
        // level (ends 200 and 350 against ideals 200 and 250).
        PackagingSchedule s = baseSchedule(1);
        addLine(s, 0, {{PA_S, 200, 200, BIG}, {PA_S, 150, 250, BIG}});
        checkScore("idealEndDateTime 100 minutes late",
                   ScoreCalculator::calculateScore(s),
                   {0, -100, -350 * 350});
    }
    {
        // maximizeJobsAssigned: a loose job (in no list) and a pooled job
        // both count as unassigned; each penalizes its duration.
        PackagingSchedule s = baseSchedule(1);
        addLine(s, 0, {{PA_S, 100}});
        addJob(s, {PA_S, 200});                           // loose
        s.unassignedJobs.push_back(addJob(s, {PB, 50}));  // pooled
        checkScore("maximizeJobsAssigned 250 unassigned minutes",
                   ScoreCalculator::calculateScore(s),
                   {0, -250, -100 * 100});
    }
    {
        // minimizeMakespan: line0 ends at 100; line1: job runs 0..200, then
        // cleanup(A small -> B) = 40 and 1000 minutes -> ends 1240. The
        // unassigned 6000-minute job lands on the medium level.
        PackagingSchedule s = baseSchedule(2);
        addLine(s, 0, {{PA_S, 100}});
        addLine(s, 1, {{PA_S, 200}, {PB, 1000}});
        addJob(s, {PA_S, 6000});
        checkScore("minimizeMakespan 100^2 + 1240^2",
                   ScoreCalculator::calculateScore(s),
                   {0, -6000, -(100 * 100 + 1240 * 1240)});
    }
    {
        // An empty line contributes nothing (no makespan term).
        PackagingSchedule s = baseSchedule(1);
        addLine(s, 0, {});
        checkScore("empty line scores 0",
                   ScoreCalculator::calculateScore(s),
                   {0, 0, 0});
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// Random mid-size instance: 4 lines over 2 shared operators, full random
// cleanup matrix, one pinned line prefix + one pinned unassigned job.
// -------------------------------------------------------------------------
PackagingSchedule buildRandomInstance(int jobCount, std::uint64_t seed)
{
    std::mt19937_64 rng(seed);
    PackagingSchedule s;
    s.fromMinute = 0;
    s.toMinute = 14 * 24 * 60;

    const int productCount = 4;
    for (int p = 0; p < productCount; ++p)
        s.products.push_back(
            {std::to_string(p), "Product " + std::to_string(p)});
    s.cleanupMinutes.assign(productCount * productCount, 0);
    for (int prev = 0; prev < productCount; ++prev)
    {
        for (int cur = 0; cur < productCount; ++cur)
        {
            s.cleanupMinutes[prev * productCount + cur]
                = prev == cur ? 0 : static_cast<Minutes>(rng() % 80);
        }
    }

    s.operators.push_back({"Operator 1"});
    s.operators.push_back({"Operator 2"});
    for (int l = 0; l < 4; ++l)
    {
        Line line;
        line.id = std::to_string(l);
        line.name = "Line " + line.id;
        line.startDateTime = 0;
        line.operatorIndex = l % 2;  // lines share operators
        s.lines.push_back(std::move(line));
    }

    for (int i = 0; i < jobCount; ++i)
    {
        Job job;
        job.id = std::to_string(i);
        job.name = "job" + job.id;
        job.productIndex = static_cast<int>(rng() % productCount);
        job.durationMinutes = 30 + static_cast<Minutes>(rng() % 270);
        job.minStartTime = 0;
        job.idealEndTime = static_cast<Minutes>(rng() % 4000);
        job.maxEndTime
            = job.idealEndTime + 500 + static_cast<Minutes>(rng() % 3000);
        s.jobs.push_back(std::move(job));
    }
    // Job 0: pinned as the prefix of line 0. Job 1: pinned unassigned
    // (prepare parks it in the pool).
    s.jobs[0].pinned = true;
    s.lines[0].jobs.push_back(0);
    s.jobs[1].pinned = true;
    // Spread the remaining jobs: lines round-robin, every 7th to the pool.
    for (int i = 2; i < jobCount; ++i)
    {
        if (i % 7 == 0)
            s.unassignedJobs.push_back(i);
        else
            s.lines[i % 4].jobs.push_back(i);
    }
    return s;
}

// -------------------------------------------------------------------------
// (b) Incremental == full recompute across a random LIST-move walk driven
// through the ScoreDirector (the exact before/mutate/after protocol the
// engine uses), against the independently coded full calculator.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Verifying incremental scoring == full recompute ==\n");
    PackagingSchedule s = buildRandomInstance(/*jobCount*/ 30, /*seed*/ 7);
    ScoreCalculator::prepare(s);

    using Descriptor = optimaru::SolutionDescriptor<PackagingSchedule>;
    FoodPackagingIncrementalScoreCalculator calculator;
    optimaru::ScoreDirector<PackagingSchedule> director(s, calculator);

    std::mt19937_64 rng(123);
    const int listCount = Descriptor::listCount(s);
    int mismatches = 0;
    const int steps = 2000;
    for (int step = 0; step < steps; ++step)
    {
        const int kind = static_cast<int>(rng() % 5);
        if (kind < 3)
        {
            // Relocate: remove a random element, insert at a random slot
            // (possibly the pool == unassign, exactly the engine's move).
            int from = static_cast<int>(rng() % listCount);
            while (Descriptor::listSize(s, from) == 0)
                from = static_cast<int>(rng() % listCount);
            const int fromPos = static_cast<int>(
                rng()
                % static_cast<std::uint64_t>(Descriptor::listSize(s, from)));
            const int element = director.removeListElement(from, fromPos);
            const int to = static_cast<int>(rng() % listCount);
            const int toPos = static_cast<int>(
                rng()
                % static_cast<std::uint64_t>(Descriptor::listSize(s, to) + 1));
            director.insertListElement(to, toPos, element);
        }
        else if (kind == 3)
        {
            // Swap two elements across (possibly equal) lists.
            int l1 = static_cast<int>(rng() % listCount);
            while (Descriptor::listSize(s, l1) == 0)
                l1 = static_cast<int>(rng() % listCount);
            int l2 = static_cast<int>(rng() % listCount);
            while (Descriptor::listSize(s, l2) == 0)
                l2 = static_cast<int>(rng() % listCount);
            const int p1 = static_cast<int>(
                rng()
                % static_cast<std::uint64_t>(Descriptor::listSize(s, l1)));
            const int p2 = static_cast<int>(
                rng()
                % static_cast<std::uint64_t>(Descriptor::listSize(s, l2)));
            if (l1 == l2 && p1 == p2)
                continue;
            const int e1 = Descriptor::getElement(s, l1, p1);
            const int e2 = Descriptor::getElement(s, l2, p2);
            director.removeListElement(l1, p1);
            director.insertListElement(l1, p1, e2);
            director.removeListElement(l2, p2);
            director.insertListElement(l2, p2, e1);
        }
        else
        {
            // 2-opt: reverse a random segment.
            int l = static_cast<int>(rng() % listCount);
            while (Descriptor::listSize(s, l) < 2)
                l = static_cast<int>(rng() % listCount);
            const int size = Descriptor::listSize(s, l);
            int first
                = static_cast<int>(rng() % static_cast<std::uint64_t>(size));
            int last
                = static_cast<int>(rng() % static_cast<std::uint64_t>(size));
            if (first > last)
                std::swap(first, last);
            if (first == last)
                continue;
            director.reverseListSegment(l, first, last);
        }

        const optimaru::HardSoftScore incremental = director.calculateScore();
        PackagingSchedule shadow = s;  // full recompute on a copy
        const optimaru::HardSoftScore full
            = toEngineScore(ScoreCalculator::calculateScore(shadow));
        if (incremental.hard != full.hard
            || std::fabs(incremental.soft - full.soft) > 1e-9)
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n",
                            step,
                            incremental.toString().c_str(),
                            full.toString().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(steps)
              + " random list moves, incremental == full recompute");

    // The pinned prefix and the pinned pool job were untouchable throughout.
    check(!s.lines[0].jobs.empty() && s.lines[0].jobs[0] == 0,
          "pinned job 0 still first on line 0 after the walk");
    check(!s.unassignedJobs.empty() && s.unassignedJobs[0] == 1,
          "pinned job 1 still parked in the pool");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) FULL_ASSERT solve: the engine cross-checks the incremental score
// against a full reset + recompute after every single move (including the
// LNS ruin/recreate path) and throws ScoreCorruptionError on divergence.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (c) FULL_ASSERT solve ==\n");
    PackagingSchedule s = buildRandomInstance(/*jobCount*/ 14, /*seed*/ 11);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    // Kick early so ruin/recreate also runs under FULL_ASSERT.
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    bool corrupted = false;
    Score final{};
    try
    {
        Solver solver(config);
        solver.solve(s);
        final = s.score;
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(s);
        check(final == recomputed, "solver-reported score == full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Pinning + the unassigned pool through a real solve.
// -------------------------------------------------------------------------
void verifyPinnedSolve()
{
    std::printf("== (d) Pinned jobs and the unassigned pool ==\n");
    PackagingSchedule s = buildRandomInstance(/*jobCount*/ 20, /*seed*/ 21);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(3);
    config.randomSeed = 5;
    Solver solver(config);
    solver.solve(s);

    check(!s.lines[0].jobs.empty() && s.lines[0].jobs[0] == 0,
          "pinned job 0 kept position 0 of line 0");
    check(s.jobs[0].lineIndex == 0, "pinned job 0 shadow lineIndex == 0");
    check(s.jobs[1].lineIndex == -1
              && std::find(s.unassignedJobs.begin(), s.unassignedJobs.end(), 1)
                     != s.unassignedJobs.end(),
          "pinned job 1 stayed unassigned (in the pool)");

    // Every unpinned job ended up in exactly one list (lines or pool).
    std::vector<int> membership(s.jobs.size(), 0);
    for (int l = 0; l < s.listCountWithPool(); ++l)
    {
        for (const int j : s.rawList(l))
            ++membership[j];
    }
    bool eachOnce = true;
    for (std::size_t j = 0; j < s.jobs.size(); ++j)
        eachOnce = eachOnce && membership[j] == 1;
    check(eachOnce, "every job in exactly one list after the solve");

    const Score recomputed = ScoreCalculator::calculateScore(s);
    check(s.score == recomputed, "reported score == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) The Line.operator variable: seeding + hill-climbing reassignment.
// -------------------------------------------------------------------------
void verifyOperatorAssignment()
{
    std::printf("== (e) Operator assignment (Line.operator stand-in) ==\n");
    {
        PackagingSchedule s = baseSchedule(5);
        addLine(s, -1, {});
        addLine(s, -1, {});
        addLine(s, -1, {});
        assignMissingOperators(s);
        check(s.lines[0].operatorIndex == 0 && s.lines[1].operatorIndex == 1
                  && s.lines[2].operatorIndex == 2,
              "seeding is injective while operators remain (0,1,2)");
    }
    {
        // Cleaning windows: line0 [10,70), line1 [12,72), line2 [300,360).
        // Sharing operator 0 between lines 0 and 1 costs 58 overlap
        // minutes; the hill climber must break that pair up (operator 1 has
        // no overlap with either).
        PackagingSchedule s = baseSchedule(2);
        addLine(s, 0, {{PB, 10}, {PA_S, 100}});
        addLine(s, 0, {{PB, 12}, {PA_S, 100}});
        addLine(s, 1, {{PB, 300}, {PA_S, 100}});
        ScoreCalculator::prepare(s);
        const std::int64_t conflictBefore
            = ScoreCalculator::analyze(s).operatorCleaningConflict;
        check(conflictBefore == 58, "shared operator costs 58 before");
        const bool changed = improveOperatorAssignment(s);
        const std::int64_t conflictAfter
            = ScoreCalculator::analyze(s).operatorCleaningConflict;
        check(changed && conflictAfter == 0,
              "hill climb resolves the operator conflict to 0");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) The Java demo instance: shape checks + a solve to 0hard.
// -------------------------------------------------------------------------
void verifyDemo()
{
    std::printf("== (f) Java demo instance (Random(37) replica) ==\n");
    PackagingSchedule s = generateDemoData();
    check(static_cast<int>(s.products.size()) == 60,
          "60 products (10 ingredients x 6)");
    check(s.lineCount() == 5 && s.operators.size() == 5 && s.jobs.size() == 100,
          "5 lines, 5 operators, 100 jobs");

    bool sortedByName = true;
    for (std::size_t i = 1; i < s.jobs.size(); ++i)
        sortedByName = sortedByName && s.jobs[i - 1].name <= s.jobs[i].name;
    check(sortedByName, "jobs sorted by name (Java post-sort)");

    bool windowsOk = true;
    for (const Job &job : s.jobs)
    {
        windowsOk = windowsOk && job.durationMinutes >= 120
                    && job.durationMinutes < 300 && job.minStartTime >= 0
                    && job.minStartTime % (24 * 60) == 0
                    && job.idealEndTime % (24 * 60) == 16 * 60
                    && job.maxEndTime >= job.idealEndTime + 24 * 60;
    }
    check(windowsOk, "job durations and time windows match the generator");

    bool cleanupOk = true;
    const int productCount = static_cast<int>(s.products.size());
    for (int prev = 0; prev < productCount; ++prev)
    {
        for (int cur = 0; cur < productCount; ++cur)
        {
            const Minutes cleanup = s.cleanup(prev, cur);
            if (prev == cur)
                cleanupOk = cleanupOk && cleanup == 0;
            else
                cleanupOk
                    = cleanupOk
                      && (cleanup == 10 || (cleanup >= 30 && cleanup < 60));
        }
    }
    check(cleanupOk, "cleanup matrix: self 0, others 10 or [30, 60)");

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(15);
    config.bestScoreLimit = {0, -1e18};  // stop at the first feasible
    config.randomSeed = 1;
    Solver solver(config);
    solver.solve(s);
    check(s.score.hard == 0,
          "demo reaches 0hard (got " + s.score.toString() + ")");
    check(s.score == ScoreCalculator::calculateScore(s),
          "reported score == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) Score mapping roundtrip + validation errors.
// -------------------------------------------------------------------------
void verifyMappingAndValidation()
{
    std::printf("== (g) Score mapping and input validation ==\n");
    {
        const Score samples[] = {{0, 0, 0},
                                 {-3, -5, -7},
                                 {0, -123456, -9999999999LL},
                                 {-42, 0, -1},
                                 {0, -900000, -9999999999LL}};
        bool ok = true;
        for (const Score &score : samples)
        {
            const Score back = fromEngineScore(toEngineScore(score));
            ok = ok && back == score;
        }
        check(ok, "HardMediumSoft <-> engine HardSoft roundtrip is exact");
        check(toEngineScore({-1, 0, 0}) < toEngineScore({0, -900000, 0})
                  && toEngineScore({0, -1, 0})
                         < toEngineScore({0, 0, -9999999999LL}),
              "level dominance survives the mapping");
    }
    {
        // Missing cleanup entry: querying the pair throws, like Java's
        // Product.getCleanupDuration.
        PackagingSchedule s = baseSchedule(1);
        s.cleanupMinutes[PB * 3 + PA_S] = kMissingCleanup;
        addLine(s, 0, {{PB, 100}, {PA_S, 100}});
        bool threw = false;
        try
        {
            ScoreCalculator::calculateScore(s);
        }
        catch (const std::invalid_argument &)
        {
            threw = true;
        }
        check(threw, "missing cleanup entry throws on first use");
    }
    {
        // A pinned job after an unpinned one is rejected.
        PackagingSchedule s = baseSchedule(1);
        addLine(s, 0, {{PA_S, 100}, {PA_S, 100, BIG, BIG, true}});
        bool threw = false;
        try
        {
            ScoreCalculator::prepare(s);
        }
        catch (const std::invalid_argument &)
        {
            threw = true;
        }
        check(threw, "pinned job after an unpinned one is rejected");
    }
    {
        // A job in two lists is rejected.
        PackagingSchedule s = baseSchedule(1);
        addLine(s, 0, {{PA_S, 100}});
        s.unassignedJobs.push_back(0);
        bool threw = false;
        try
        {
            ScoreCalculator::prepare(s);
        }
        catch (const std::invalid_argument &)
        {
            threw = true;
        }
        check(threw, "job in two lists is rejected");
    }
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyConstraints();
    verifyIncremental();
    verifyFullAssertSolve();
    verifyPinnedSolve();
    verifyOperatorAssignment();
    verifyDemo();
    verifyMappingAndValidation();

    if (failures == 0)
    {
        std::printf("All food-packaging checks PASSED.\n");
        return 0;
    }
    std::printf("%d food-packaging check(s) FAILED.\n", failures);
    return 1;
}

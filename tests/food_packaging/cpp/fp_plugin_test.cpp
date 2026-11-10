// Plugin-assembly (fp/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedObjective
//       reproduces the monolithic score bit-for-bit on 200 random list
//       layouts (unassigned jobs included)
//   (b) incremental score == full recompute across 1500 random sequence moves
//       driven through the engine's Editor protocol (plugin path)
//   (c) constraint_totals() explains exactly the reported score
//   (d) weight / level overrides
//   (e) a 2-second solve(schedule, calculator) under ScoreChecks::Full
//   (f) each component's raw cost, hand-calculated: a positive and a zero
//       case per constraint (raw 0 = no violation)
//   (g) the catalog: info() off the default assembly == the components'
//       kDefaultWeight / kDefaultLevel; the level and knob guards
//   (h) the medium level (afterIdealEndTimeCost, unassignedJobsCost): the
//       10^10 fold, hard() / soft() / weight, and solve() reporting the
//       assembly's levels
//   (i) incremental == full recompute with each medium term overridden
//   (j) on_new_best on the plugin path reports the same levels as
//       schedule.score
//   (k) repeated components and the offset / clip knobs: a hand-calculated
//       repeat, threshold and literal offsets by hand (the per-line
//       constraints evaluate every line, an empty one included, never the
//       unassigned pool), the globals' totals shaped once, the literal
//       formula refused on hard at reset and kept on soft (and on the
//       into_soft medium), incremental == a fresh recompute over a random
//       sequence walk and a ScoreChecks::Full solve with repeated, shifted components
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fp/constraints.hpp"
#include "fp/demo_data.hpp"
#include "fp/domain.hpp"
#include "fp/score_calculator.hpp"
#include "fp/solver.hpp"
#include "optimaru/model/editor.hpp"

using namespace fp;

namespace {

int failures = 0;

void check(bool ok, const std::string& label) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}

bool sameScore(const Score& a, const Score& b) {
    return a.hard == b.hard && a.medium == b.medium && a.soft == b.soft;
}

Score scoreWith(DecomposedCalculator& calculator, PackagingSchedule& s) {
    ScoreCalculator::prepare(s);  // idempotent; the components require it
    calculator.rebuild(s);
    return fromEngineScore(calculator.score());
}

// Spreads every job over the lines at random, writing straight into the
// schedule's own lists. `poolRate` of them go to the unassigned pool list and
// another `poolRate` into NO list at all — the state a solution starts in,
// which the score treats as unassigned just the same.
void randomizeLists(PackagingSchedule& s, std::mt19937_64& rng, double poolRate) {
    const int jobCount = static_cast<int>(s.jobs.size());
    for (Line& line : s.lines) line.jobs.clear();
    s.unassignedJobs.clear();
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (int j = 0; j < jobCount; ++j) {
        const double roll = unit(rng);
        if (roll < poolRate) {
            s.unassignedJobs.push_back(j);
            continue;
        }
        if (roll < 2 * poolRate) continue;  // in no list at all
        s.lines[rng() % s.lines.size()].jobs.push_back(j);
    }
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute.
// -------------------------------------------------------------------------
void verifyAssemblyParity() {
    std::printf("== (a) Default assembly == monolithic score ==\n");
    PackagingSchedule s = generateDemoData(/*lines*/ 4, /*jobs*/ 30);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    int mismatches = 0;
    const int rounds = 200;
    for (int round = 0; round < rounds; ++round) {
        randomizeLists(s, rng, /*poolRate*/ 0.10);
        const Score plugin = scoreWith(calculator, s);
        const Score monolithic = ScoreCalculator::score(s);
        if (!sameScore(plugin, monolithic)) {
            if (++mismatches <= 3) {
                std::printf("  MISMATCH @round %d: plugin=%s monolithic=%s\n", round,
                            plugin.str().c_str(), monolithic.str().c_str());
            }
        }
    }
    check(mismatches == 0, std::to_string(rounds) + " random list layouts score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of sequence
// mutations, driven through the Editor. The unassigned pool is one of
// the engine's lists here, so a "relocate to the pool" is an ordinary move.
// -------------------------------------------------------------------------
void verifyIncremental() {
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    PackagingSchedule s = generateDemoData(/*lines*/ 4, /*jobs*/ 24);
    ScoreCalculator::prepare(s);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::Editor<PackagingSchedule> editor(s, calculator);

    using Descriptor = optimaru::SolutionTraits<PackagingSchedule>;
    const int list_count = Descriptor::list_count(s);
    const int element_count = Descriptor::element_count(s);

    std::mt19937_64 rng(2024);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    // Where each engine element currently sits (list, position is looked up).
    std::vector<int> list_of(element_count, -1);
    for (int l = 0; l < list_count; ++l) {
        for (int p = 0; p < Descriptor::list_size(s, l); ++p)
            list_of[Descriptor::element_at(s, l, p)] = l;
    }
    auto indexWithin = [&](int list, int element) {
        for (int p = 0; p < Descriptor::list_size(s, list); ++p) {
            if (Descriptor::element_at(s, list, p) == element) return p;
        }
        return -1;
    };

    int mismatches = 0;
    const int steps = 1500;
    for (int step = 0; step < steps; ++step) {
        if (element_count == 0) break;
        const int element = static_cast<int>(rng() % element_count);
        if (unit(rng) < 0.8) {
            // Relocate to a random (list, position).
            const int from = list_of[element];
            if (from >= 0) editor.remove(from, indexWithin(from, element));
            const int to = static_cast<int>(rng() % list_count);
            const int position = static_cast<int>(
                    rng() % static_cast<unsigned>(Descriptor::list_size(s, to) + 1));
            editor.insert(to, position, element);
            list_of[element] = to;
        } else {
            // Reverse a random segment (2-opt).
            const int list = static_cast<int>(rng() % list_count);
            const int size = Descriptor::list_size(s, list);
            if (size < 2) continue;
            int first = static_cast<int>(rng() % size);
            int last = static_cast<int>(rng() % size);
            if (first > last) std::swap(first, last);
            editor.reverse(list, first, last);
        }

        const Score incremental = fromEngineScore(editor.score());
        const Score full = ScoreCalculator::score(s);
        if (!sameScore(full, incremental)) {
            if (++mismatches <= 3) {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n", step,
                            incremental.str().c_str(), full.str().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(steps) + " random list moves, incremental == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) constraint_totals(): the per-constraint breakdown adds up, and each
// component isolates exactly its own term of the monolithic breakdown.
// -------------------------------------------------------------------------
void verifyConstraintTotals() {
    std::printf("== (c) Per-component totals vs the monolithic breakdown ==\n");
    PackagingSchedule s = generateDemoData(/*lines*/ 4, /*jobs*/ 24);
    ScoreCalculator::prepare(s);
    std::mt19937_64 rng(5);
    randomizeLists(s, rng, /*poolRate*/ 0.15);

    const ConstraintBreakdown breakdown = ScoreCalculator::analyze(s);

    struct Case {
        const char* label;
        std::int64_t expected;
        Score (*score)(PackagingSchedule&);
    };

    {
        DecomposedCalculator calculator;
        calculator.emplace<MaxEndTimeCost>();
        const Score score = scoreWith(calculator, s);
        check(score.hard == -breakdown.maxEndTimeCost && score.medium == 0 && score.soft == 0,
              "maxEndTimeCost alone == its breakdown term");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<OperatorCleaningConflictCost>();
        const Score score = scoreWith(calculator, s);
        check(score.hard == -breakdown.operatorCleaningConflictCost && score.medium == 0 &&
                      score.soft == 0,
              "operatorCleaningConflictCost alone == its breakdown term");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<AfterIdealEndTimeCost>();
        const Score score = scoreWith(calculator, s);
        check(score.hard == 0 && score.medium == -breakdown.afterIdealEndTimeCost &&
                      score.soft == 0,
              "afterIdealEndTimeCost alone == its breakdown term");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<UnassignedJobsCost>();
        const Score score = scoreWith(calculator, s);
        check(score.hard == 0 && score.medium == -breakdown.unassignedJobsCost && score.soft == 0,
              "unassignedJobsCost alone == its breakdown term");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<SquaredMakespanCost>();
        const Score score = scoreWith(calculator, s);
        check(score.hard == 0 && score.medium == 0 && score.soft == -breakdown.squaredMakespanCost,
              "squaredMakespanCost alone == its breakdown term");
    }
    {
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        const Score score = scoreWith(calculator, s);
        // A medium total is in medium units (cost is not multiplied by the
        // fold); fold it here to compare with the engine score.
        double hard = 0.0;
        double soft = 0.0;
        for (const optimaru::ConstraintTotal& total : calculator.constraint_totals()) {
            if (total.level == optimaru::Level::Hard)
                hard += total.cost;
            else if (total.level == optimaru::Level::Medium)
                soft += total.cost * static_cast<double>(kMediumUnit);
            else
                soft += total.cost;
        }
        const optimaru::Score engineScore = toEngineScore(score);
        check(std::fabs(-hard - static_cast<double>(engineScore.hard)) < 1e-9 &&
                      std::fabs(-soft - engineScore.soft) < 1e-3,
              "per-constraint totals sum to the reported score");
        check(calculator.constraint_totals().size() == 5, "one total per assembled component");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides.
// -------------------------------------------------------------------------
void verifyOverrides() {
    std::printf("== (d) Weight / level overrides ==\n");
    PackagingSchedule s = generateDemoData(/*lines*/ 3, /*jobs*/ 12);
    ScoreCalculator::prepare(s);
    std::mt19937_64 rng(7);
    randomizeLists(s, rng, /*poolRate*/ 0.25);
    const ConstraintBreakdown breakdown = ScoreCalculator::analyze(s);

    {
        DecomposedCalculator calculator;
        calculator.emplace<MaxEndTimeCost>(3.0);
        const Score score = scoreWith(calculator, s);
        check(score.hard == -3 * breakdown.maxEndTimeCost, "maxEndTimeCost re-weighted to 3");
    }
    {
        // Demoted to soft at weight 1: the same minutes land on the soft
        // level instead of the hard one.
        DecomposedCalculator calculator;
        auto component = std::make_unique<MaxEndTimeCost>();
        component->level = optimaru::Level::Soft;
        calculator.add(std::move(component));
        const Score score = scoreWith(calculator, s);
        check(score.hard == 0 && score.medium == 0 && score.soft == -breakdown.maxEndTimeCost,
              "maxEndTimeCost demoted to soft");
    }
    {
        // The same edit by registry name through the assembly.
        DecomposedCalculator calculator;
        defaultConstraints().only({"maxEndTimeCost"}).soft("maxEndTimeCost").into(calculator);
        const Score score = scoreWith(calculator, s);
        check(score.hard == 0 && score.medium == 0 && score.soft == -breakdown.maxEndTimeCost,
              "only().soft(\"maxEndTimeCost\") == the by-hand demotion");
    }
    {
        // exponent 2 shapes each line's raw lateness before the weight; the
        // global conflict shapes its single total.
        DecomposedCalculator calculator;
        auto conflict = std::make_unique<OperatorCleaningConflictCost>();
        conflict->exponent = 2.0;
        calculator.add(std::move(conflict));
        const Score score = scoreWith(calculator, s);
        check(score.hard == -breakdown.operatorCleaningConflictCost *
                                    breakdown.operatorCleaningConflictCost,
              "operatorCleaningConflictCost exponent 2 squares the total");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) ScoreChecks::Full solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve() {
    std::printf("== (e) FULL_ASSERT plugin solve ==\n");
    PackagingSchedule s = generateDemoData(/*lines*/ 3, /*jobs*/ 12);

    Solver::Config config;
    config.time_limit = std::chrono::seconds(2);
    config.seed = 3;
    config.score_checks = optimaru::ScoreChecks::Full;
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    bool corrupted = false;
    try {
        Solver solver(config);
        solver.solve(s, calculator);
    } catch (const optimaru::ScoreMismatchError& e) {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted) {
        const Score recomputed = ScoreCalculator::score(s);
        check(sameScore(s.score, recomputed), "plugin-reported score == monolithic full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// Hand-built schedules for (f) and (h): the fp_test products (0 = A small,
// 2 = B; cleanup B -> A small = 60, A small -> A small = 0), every line
// starting at minute 0.
// -------------------------------------------------------------------------
constexpr Minutes BIG = 1000000000LL;  // a deadline that never fires
constexpr int PA_S = 0;
constexpr int PB = 2;

struct JobSpec {
    int product;
    Minutes duration;
    Minutes idealEnd = BIG;
    Minutes maxEnd = BIG;
};

PackagingSchedule handSchedule(int operatorCount) {
    PackagingSchedule s;
    s.fromMinute = 0;
    s.toMinute = 14 * 24 * 60;
    s.products = {{"1", "Product A small"}, {"2", "Product A large"}, {"3", "Product B"}};
    s.cleanupMinutes = {0, 0, 40, 5, 0, 40, 60, 60, 0};
    for (int o = 0; o < operatorCount; ++o)
        s.operators.push_back({"Operator " + std::to_string(o + 1)});
    return s;
}

int addJob(PackagingSchedule& s, const JobSpec& spec) {
    Job job;
    job.id = std::to_string(s.jobs.size());
    job.name = "job" + job.id;
    job.productIndex = spec.product;
    job.durationMinutes = spec.duration;
    job.idealEndTime = spec.idealEnd;
    job.maxEndTime = spec.maxEnd;
    s.jobs.push_back(std::move(job));
    return static_cast<int>(s.jobs.size()) - 1;
}

void addLine(PackagingSchedule& s, int operator_index, const std::vector<JobSpec>& specs) {
    Line line;
    line.id = std::to_string(s.lines.size());
    line.name = "Line " + line.id;
    line.operator_index = operator_index;
    for (const JobSpec& spec : specs) line.jobs.push_back(addJob(s, spec));
    s.lines.push_back(std::move(line));
}

// -------------------------------------------------------------------------
// (f) Each component's raw cost on hand-built schedules: one violating
// (positive) case and one clean (zero) case per constraint, read through
// constraint_costs() — the raw numbers before weight, exponent and level. A
// unit constraint reports one cost per list (the lines, then the unassigned
// pool); a global one reports its single total.
// -------------------------------------------------------------------------
std::vector<double> rawCosts(PackagingSchedule& schedule, const std::string& name) {
    ScoreCalculator::prepare(schedule);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.rebuild(schedule);
    calculator.score();
    for (const auto& entry : calculator.constraint_costs()) {
        if (entry.name == name) return entry.unit_costs;
    }
    return {};
}

void verifyRawCostsByHand() {
    std::printf("== (f) Raw costs by hand, positive and zero ==\n");
    using Costs = std::vector<double>;
    {
        // Line 0: J0 runs 0..200 (deadline 200, on time), J1 200..350
        // (deadline 250) -> 100 late. Line 1 has no deadline in reach.
        PackagingSchedule s = handSchedule(2);
        addLine(s, 0, {{PA_S, 200, BIG, 200}, {PA_S, 150, BIG, 250}});
        addLine(s, 1, {{PB, 100}});
        check(rawCosts(s, "maxEndTimeCost") == Costs{100.0, 0.0, 0.0},
              "maxEndTimeCost: [100, 0, pool 0] (J1 ends 350, due 250)");
    }
    {
        // The same chain with J1 due exactly when it ends.
        PackagingSchedule s = handSchedule(2);
        addLine(s, 0, {{PA_S, 200, BIG, 200}, {PA_S, 150, BIG, 350}});
        addLine(s, 1, {{PB, 100}});
        check(rawCosts(s, "maxEndTimeCost") == Costs{0.0, 0.0, 0.0},
              "maxEndTimeCost: [0, 0, 0] every job ends by its deadline");
    }
    {
        // Lines 0 and 1 share operator 0: cleaning windows [10, 70) and
        // [30, 90) overlap 40 minutes. Line 2 (operator 1) cleans [5, 65)
        // with no same-operator partner.
        PackagingSchedule s = handSchedule(2);
        addLine(s, 0, {{PB, 10}, {PA_S, 100}});
        addLine(s, 0, {{PB, 30}, {PA_S, 100}});
        addLine(s, 1, {{PB, 5}, {PA_S, 100}});
        check(rawCosts(s, "operatorCleaningConflictCost") == Costs{40.0},
              "operatorCleaningConflictCost: [40] overlap minutes");
    }
    {
        // The same lines, each with its own operator.
        PackagingSchedule s = handSchedule(3);
        addLine(s, 0, {{PB, 10}, {PA_S, 100}});
        addLine(s, 1, {{PB, 30}, {PA_S, 100}});
        addLine(s, 2, {{PB, 5}, {PA_S, 100}});
        check(rawCosts(s, "operatorCleaningConflictCost") == Costs{0.0},
              "operatorCleaningConflictCost: [0] no shared operator");
    }
    {
        // Ends 200 and 350 against ideal ends 200 and 250 -> 100 minutes.
        PackagingSchedule s = handSchedule(2);
        addLine(s, 0, {{PA_S, 200, 200, BIG}, {PA_S, 150, 250, BIG}});
        addLine(s, 1, {{PB, 100, 100, BIG}});
        check(rawCosts(s, "afterIdealEndTimeCost") == Costs{100.0, 0.0, 0.0},
              "afterIdealEndTimeCost: [100, 0, pool 0]");
    }
    {
        PackagingSchedule s = handSchedule(2);
        addLine(s, 0, {{PA_S, 200, 200, BIG}, {PA_S, 150, 350, BIG}});
        addLine(s, 1, {{PB, 100, 100, BIG}});
        check(rawCosts(s, "afterIdealEndTimeCost") == Costs{0.0, 0.0, 0.0},
              "afterIdealEndTimeCost: [0, 0, 0] every job ends by its ideal "
              "time");
    }
    {
        // A loose job (in no list, 200 min) and a pooled job (50 min).
        PackagingSchedule s = handSchedule(1);
        addLine(s, 0, {{PA_S, 100}});
        addJob(s, {PA_S, 200});
        s.unassignedJobs.push_back(addJob(s, {PB, 50}));
        check(rawCosts(s, "unassignedJobsCost") == Costs{250.0},
              "unassignedJobsCost: [250] (loose 200 + pooled 50)");
    }
    {
        PackagingSchedule s = handSchedule(1);
        addLine(s, 0, {{PA_S, 100}, {PB, 50}});
        check(rawCosts(s, "unassignedJobsCost") == Costs{0.0},
              "unassignedJobsCost: [0] every job on a line");
    }
    {
        // Line 0: B 0..100, cleanup B -> A small 60, A small 160..260 ->
        // 260^2 = 67600. Line 1 is empty.
        PackagingSchedule s = handSchedule(2);
        addLine(s, 0, {{PB, 100}, {PA_S, 100}});
        addLine(s, 1, {});
        check(rawCosts(s, "squaredMakespanCost") == Costs{67600.0, 0.0, 0.0},
              "squaredMakespanCost: [260^2, 0, pool 0]");
    }
    {
        // Every job in the pool: no line is busy.
        PackagingSchedule s = handSchedule(2);
        addLine(s, 0, {});
        addLine(s, 1, {});
        s.unassignedJobs.push_back(addJob(s, {PB, 100}));
        check(rawCosts(s, "squaredMakespanCost") == Costs{0.0, 0.0, 0.0},
              "squaredMakespanCost: [0, 0, 0] with empty lines");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) The catalog read off the default assembly is exactly the components'
// constants, and the level/weight guards hold on the domain's assembly.
// -------------------------------------------------------------------------
bool throwsInvalid(const std::function<void()>& edit) {
    try {
        edit();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

template <typename Component>
bool rowIs(const optimaru::ConstraintInfo& row, const char* name, optimaru::Level level,
           double weight, bool is_global) {
    return row.name == name && row.default_level == Component::kDefaultLevel &&
           row.default_weight == Component::kDefaultWeight && row.default_level == level &&
           row.default_weight == weight && row.level_flippable && !row.reward &&
           row.is_global == is_global && row.default_enabled;
}

void verifyCatalog() {
    std::printf("== (g) Catalog and guards ==\n");
    using optimaru::Level;
    const auto rows = defaultConstraints().info();
    check(rows.size() == 5, "5 catalog rows");
    if (rows.size() != 5) return;
    // Unit constraints first, then the globals: the list_constraints() order.
    check(rowIs<MaxEndTimeCost>(rows[0], "maxEndTimeCost", Level::Hard, 1.0, false) &&
                  rowIs<AfterIdealEndTimeCost>(rows[1], "afterIdealEndTimeCost", Level::Medium, 1.0,
                                               false) &&
                  rowIs<SquaredMakespanCost>(rows[2], "squaredMakespanCost", Level::Soft, 1.0,
                                             false) &&
                  rowIs<OperatorCleaningConflictCost>(rows[3], "operatorCleaningConflictCost",
                                                      Level::Hard, 1.0, true) &&
                  rowIs<UnassignedJobsCost>(rows[4], "unassignedJobsCost", Level::Medium, 1.0,
                                            true),
          "info(): maxEndTimeCost hard 1, afterIdealEndTimeCost medium 1, "
          "squaredMakespanCost soft 1, operatorCleaningConflictCost hard 1 "
          "(global), unassignedJobsCost medium 1 (global); all flippable, no "
          "reward, all enabled");
    std::vector<std::string> infoNames;
    for (const auto& row : rows) infoNames.push_back(row.name);
    check(infoNames == defaultConstraints().names(), "info() rows in names() order");
    check(!throwsInvalid([] { defaultConstraints().soft("maxEndTimeCost"); }) &&
                  !throwsInvalid([] { defaultConstraints().hard("squaredMakespanCost"); }) &&
                  !throwsInvalid([] { defaultConstraints().hard("afterIdealEndTimeCost"); }) &&
                  !throwsInvalid([] { defaultConstraints().soft("unassignedJobsCost"); }),
          "every level flips (medium to hard or soft too)");
    check(throwsInvalid([] { defaultConstraints().weight("squaredMakespanCost", -2.0); }),
          "a negative weight is refused");
    check(throwsInvalid([] { defaultConstraints().exponent("maxEndTimeCost", 0.0); }),
          "a non-positive exponent is refused");
    check(throwsInvalid([] { defaultConstraints().weight("noSuchConstraint", 2.0); }),
          "an unknown registry name is refused");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) The medium level: afterIdealEndTimeCost and unassignedJobsCost.
// -------------------------------------------------------------------------
optimaru::DecomposedObjective<PackagingSchedule>::LevelScores levelsWith(
        ConstraintAssembly assembly, PackagingSchedule& schedule) {
    ScoreCalculator::prepare(schedule);
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    calculator.rebuild(schedule);
    calculator.score();
    return calculator.level_scores();
}

bool levelsAre(const optimaru::DecomposedObjective<PackagingSchedule>::LevelScores& levels,
               std::int64_t hard, double medium, double soft) {
    return levels.hard == hard && levels.medium == medium && levels.soft == soft;
}

void verifyMediumLevel() {
    std::printf("== (h) Medium level ==\n");
    // Line 0: J0 0..200 (ideal 200), J1 200..350 (ideal 250) -> 100 minutes
    // after the ideal end; one loose 50-minute job -> 50 unassigned minutes;
    // makespan 350^2 = 122500 soft.
    PackagingSchedule s = handSchedule(1);
    addLine(s, 0, {{PA_S, 200, 200, BIG}, {PA_S, 150, 250, BIG}});
    addJob(s, {PB, 50});
    const Score monolithic = ScoreCalculator::score(s);
    check(sameScore(monolithic, Score{0, -150, -122500}),
          "the hand schedule: 0hard/-150medium/-122500soft");

    {
        ScoreCalculator::prepare(s);
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        calculator.rebuild(s);
        const optimaru::Score engine = calculator.score();
        check(engine.hard == 0 &&
                      engine.soft == -122500.0 - 150.0 * static_cast<double>(kMediumUnit),
              "default: medium folded into soft x 10^10");
        const optimaru::Score adapter = toEngineScore(monolithic);
        check(engine.hard == adapter.hard && engine.soft == adapter.soft,
              "default: the fold equals the monolithic Score adapter");
        check(levelsAre(calculator.level_scores(), 0, -150.0, -122500.0),
              "levelScores() reports the unfolded levels");
    }
    check(levelsAre(levelsWith(defaultConstraints().hard("afterIdealEndTimeCost"), s), -100, -50.0,
                    -122500.0),
          "afterIdealEndTimeCost hard(): one hard unit per raw minute");
    check(levelsAre(levelsWith(defaultConstraints().soft("afterIdealEndTimeCost"), s), 0, -50.0,
                    -122600.0),
          "afterIdealEndTimeCost soft(): one soft unit per raw minute, "
          "outside the fold");
    check(levelsAre(levelsWith(defaultConstraints().weight("afterIdealEndTimeCost", 3), s), 0,
                    -350.0, -122500.0),
          "afterIdealEndTimeCost weight 3 stays in medium units");
    check(levelsAre(levelsWith(defaultConstraints().hard("unassignedJobsCost"), s), -50, -100.0,
                    -122500.0),
          "unassignedJobsCost hard(): one hard unit per raw minute");
    check(levelsAre(levelsWith(defaultConstraints().soft("unassignedJobsCost"), s), 0, -100.0,
                    -122550.0),
          "unassignedJobsCost soft(): one soft unit per raw minute, outside "
          "the fold");
    check(levelsAre(levelsWith(defaultConstraints().weight("unassignedJobsCost", 2), s), 0, -200.0,
                    -122500.0),
          "unassignedJobsCost weight 2 stays in medium units");
    check(levelsAre(levelsWith(defaultConstraints().level("squaredMakespanCost",
                                                          optimaru::Level::Medium),
                               s),
                    0, -122650.0, 0.0),
          "level(Medium): squaredMakespanCost moves onto medium");
    check(levelsAre(levelsWith(defaultConstraints()
                                       .level("squaredMakespanCost", optimaru::Level::Medium)
                                       .weight("squaredMakespanCost", 0.5),
                               s),
                    0, -61400.0, 0.0),
          "a weight on a moved constraint is in medium units");
    {
        // solve(schedule, calculator) reports the assembly's levels. One
        // line, one 100-minute job: assigned it costs 100^2 = 10000 soft
        // (squaredMakespanCost); with unassignedJobsCost demoted to soft,
        // leaving it out costs only 100 soft, so the solver leaves it in the
        // pool and those minutes land on SOFT (not medium).
        PackagingSchedule one = handSchedule(1);
        addLine(one, 0, {});
        addJob(one, {PA_S, 100});
        Solver::Config config;
        config.time_limit = std::chrono::milliseconds(300);
        config.seed = 1;
        DecomposedCalculator calculator;
        ConstraintAssembly{}
                .emplace<UnassignedJobsCost>()
                .emplace<SquaredMakespanCost>()
                .soft("unassignedJobsCost")
                .into(calculator);
        Solver(config).solve(one, calculator);
        check(sameScore(one.score, Score{0, 0, -100}),
              "solve() with unassignedJobsCost demoted: 0hard/0medium/-100soft "
              "(got " +
                      one.score.str() + ")");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) Incremental == full recompute with each medium term overridden,
// through the Editor's sequence protocol (relocations, including to the
// pool, and 2-opt reversals).
// -------------------------------------------------------------------------
void verifyMediumIncremental() {
    std::printf("== (i) Medium overrides: incremental == full ==\n");
    // Each variant names the medium constraint it overrides (empty for the
    // default assembly), so the walk can prove it reaches states where THAT
    // term costs something.
    struct Variant {
        std::string label;
        std::string overridden;
        std::function<ConstraintAssembly()> assembly;
    };
    const std::vector<Variant> variants{
            {"default", "", [] { return defaultConstraints(); }},
            {"afterIdealEndTimeCost hard", "afterIdealEndTimeCost",
             [] { return defaultConstraints().hard("afterIdealEndTimeCost"); }},
            {"afterIdealEndTimeCost soft", "afterIdealEndTimeCost",
             [] { return defaultConstraints().soft("afterIdealEndTimeCost"); }},
            {"afterIdealEndTimeCost weight 3", "afterIdealEndTimeCost",
             [] { return defaultConstraints().weight("afterIdealEndTimeCost", 3); }},
            {"unassignedJobsCost hard", "unassignedJobsCost",
             [] { return defaultConstraints().hard("unassignedJobsCost"); }},
            {"unassignedJobsCost soft", "unassignedJobsCost",
             [] { return defaultConstraints().soft("unassignedJobsCost"); }},
            {"unassignedJobsCost weight 2", "unassignedJobsCost",
             [] { return defaultConstraints().weight("unassignedJobsCost", 2); }},
            {"squaredMakespanCost on medium", "squaredMakespanCost",
             [] {
                 return defaultConstraints().level("squaredMakespanCost", optimaru::Level::Medium);
             }},
    };
    // The raw unit sum of `name` on the calculator's working solution.
    auto rawSum = [](const DecomposedCalculator& calculator, const std::string& name) {
        double sum = 0.0;
        for (const auto& entry : calculator.constraint_costs()) {
            if (entry.name != name) continue;
            for (const double cost : entry.unit_costs) sum += cost;
        }
        return sum;
    };
    using Descriptor = optimaru::SolutionTraits<PackagingSchedule>;
    for (const auto& variant : variants) {
        PackagingSchedule s = generateDemoData(/*lines*/ 3, /*jobs*/ 18);
        ScoreCalculator::prepare(s);
        DecomposedCalculator calculator;
        variant.assembly().into(calculator);
        optimaru::Editor<PackagingSchedule> editor(s, calculator);
        const int list_count = Descriptor::list_count(s);
        const int element_count = Descriptor::element_count(s);
        std::vector<int> list_of(element_count, -1);
        for (int l = 0; l < list_count; ++l) {
            for (int p = 0; p < Descriptor::list_size(s, l); ++p)
                list_of[Descriptor::element_at(s, l, p)] = l;
        }
        auto indexWithin = [&](int list, int element) {
            for (int p = 0; p < Descriptor::list_size(s, list); ++p) {
                if (Descriptor::element_at(s, list, p) == element) return p;
            }
            return -1;
        };
        std::mt19937_64 rng(41);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        int mismatches = 0;
        int mediumSteps = 0;
        int overriddenSteps = 0;
        for (int step = 0; step < 800 && element_count > 0; ++step) {
            const int element = static_cast<int>(rng() % element_count);
            if (unit(rng) < 0.8) {
                const int from = list_of[element];
                if (from >= 0) editor.remove(from, indexWithin(from, element));
                const int to = static_cast<int>(rng() % list_count);
                const int position = static_cast<int>(
                        rng() % static_cast<unsigned>(Descriptor::list_size(s, to) + 1));
                editor.insert(to, position, element);
                list_of[element] = to;
            } else {
                const int list = static_cast<int>(rng() % list_count);
                const int size = Descriptor::list_size(s, list);
                if (size < 2) continue;
                int first = static_cast<int>(rng() % size);
                int last = static_cast<int>(rng() % size);
                if (first > last) std::swap(first, last);
                editor.reverse(list, first, last);
            }
            const optimaru::Score incremental = editor.score();
            DecomposedCalculator fresh;
            variant.assembly().into(fresh);
            fresh.rebuild(s);
            const optimaru::Score full = fresh.score();
            if (fresh.level_scores().medium != 0.0) ++mediumSteps;
            if (!variant.overridden.empty() && rawSum(fresh, variant.overridden) > 0.0)
                ++overriddenSteps;
            if (incremental.hard != full.hard || std::fabs(incremental.soft - full.soft) > 1e-3)
                ++mismatches;
        }
        check(mismatches == 0, variant.label + ": 800 random list moves, incremental == full");
        if (variant.overridden.empty()) {
            check(mediumSteps > 0, variant.label + ": the walk visits medium states");
        } else {
            // The other medium constraint can keep the medium level non-zero
            // on its own, so count the overridden term itself.
            check(overriddenSteps > 0, variant.label +
                                               ": the walk visits states where the overridden term "
                                               "is non-zero");
        }
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (j) on_new_best on the plugin path reports the levels schedule.score reports.
// -------------------------------------------------------------------------
// With squaredMakespanCost at weight 10^4 the soft total passes 10^10, which a
// decoded fold would split into medium + (soft mod 10^10). The last new best
// is the returned solution, so its report must equal schedule.score exactly.
void verifyPluginCallbackLevels() {
    std::printf("== (j) Plugin-path onNewBest levels == schedule.score ==\n");
    PackagingSchedule s = generateDemoData(/*lines*/ 3, /*jobs*/ 18);

    Solver::Config config;
    config.time_limit = std::chrono::seconds(10);
    config.step_limit = 3000;
    config.seed = 5;
    std::vector<Score> reports;
    config.on_new_best = [&](std::int64_t, const Score& best) { reports.push_back(best); };

    constexpr double kWeight = 1e4;
    DecomposedCalculator calculator;
    defaultConstraints().weight("squaredMakespanCost", kWeight).into(calculator);
    Solver(config).solve(s, calculator);

    check(!reports.empty(), "onNewBest fired");
    check(s.score.soft <= -kMediumUnit, "the final soft total is 10^10 or more");
    check(!reports.empty() && reports.back() == s.score,
          "the last onNewBest score == schedule.score (" + s.score.str() + ")");
    // Folded back, the reports are the engine's successive bests: strictly
    // improving in its (hard, soft) order.
    bool improving = true;
    for (std::size_t i = 1; i < reports.size(); ++i)
        improving = improving && toEngineScore(reports[i - 1]) < toEngineScore(reports[i]);
    check(improving, std::to_string(reports.size()) +
                             " reports, strictly improving once folded into the engine "
                             "score");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (k) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with add_instances(), as the Python bindings
// build it from a pick list.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string& name) {
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

// Line 0: J0 0..200 (ideal 200), J1 200..350 (ideal 250); line 1 empty; a
// 50-minute job in the unassigned pool. Raw per unit (line 0, line 1, pool):
// afterIdealEndTimeCost [100, 0, 0], squaredMakespanCost [350^2, 0, 0],
// maxEndTimeCost [0, 0, 0] (evaluated on the two lines); the globals
// unassignedJobsCost [50] and operatorCleaningConflictCost [0].
PackagingSchedule repeatSchedule() {
    PackagingSchedule s = handSchedule(2);
    addLine(s, 0, {{PA_S, 200, 200, BIG}, {PA_S, 150, 250, BIG}});
    addLine(s, 1, {});
    s.unassignedJobs.push_back(addJob(s, {PB, 50}));
    ScoreCalculator::prepare(s);
    return s;
}

ConstraintAssembly repeatedAssembly() {
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("squaredMakespanCost", 10000.0);
    assembly.add_instances(std::move(copyOf("squaredMakespanCost")
                                             .weight("squaredMakespanCost", 0.25)
                                             .offset("squaredMakespanCost", 40000.0)
                                             .clip("squaredMakespanCost", false)));
    // The literal copies of the hard constraints sit on soft: hard refuses
    // the literal formula with an offset (hard 0 must mean no violation).
    assembly.add_instances(std::move(copyOf("maxEndTimeCost")
                                             .soft("maxEndTimeCost")
                                             .weight("maxEndTimeCost", 2.0)
                                             .offset("maxEndTimeCost", 1.0)
                                             .clip("maxEndTimeCost", false)));
    assembly.add_instances(std::move(
            copyOf("maxEndTimeCost").weight("maxEndTimeCost", 2.0).offset("maxEndTimeCost", 1.0)));
    assembly.add_instances(std::move(copyOf("afterIdealEndTimeCost")
                                             .exponent("afterIdealEndTimeCost", 2.0)
                                             .offset("afterIdealEndTimeCost", 30.0)));
    assembly.add_instances(std::move(copyOf("afterIdealEndTimeCost")
                                             .soft("afterIdealEndTimeCost")
                                             .weight("afterIdealEndTimeCost", 3.0)
                                             .offset("afterIdealEndTimeCost", -5.0)
                                             .clip("afterIdealEndTimeCost", false)));
    assembly.add_instances(std::move(copyOf("operatorCleaningConflictCost")
                                             .soft("operatorCleaningConflictCost")
                                             .weight("operatorCleaningConflictCost", 2.0)
                                             .offset("operatorCleaningConflictCost", 3.0)
                                             .clip("operatorCleaningConflictCost", false)));
    assembly.add_instances(std::move(copyOf("operatorCleaningConflictCost")
                                             .weight("operatorCleaningConflictCost", 2.0)
                                             .offset("operatorCleaningConflictCost", 3.0)));
    assembly.add_instances(std::move(copyOf("unassignedJobsCost")
                                             .soft("unassignedJobsCost")
                                             .offset("unassignedJobsCost", -10.0)));
    return assembly;
}

void verifyRepeatsAndOffsets() {
    std::printf("== (k) Repeated components, offset / clip ==\n");
    {
        PackagingSchedule s = repeatSchedule();
        ConstraintAssembly twice = copyOf("squaredMakespanCost");
        twice.add_instances(std::move(copyOf("squaredMakespanCost")
                                              .weight("squaredMakespanCost", 0.5)
                                              .exponent("squaredMakespanCost", 0.5)));
        DecomposedCalculator calculator;
        std::move(twice).into(calculator);
        calculator.rebuild(s);
        calculator.score();
        check(levelsAre(calculator.level_scores(), 0, 0.0, -(122500.0 + 0.5 * 350)),
              "makespan^2 + 0.5 x sqrt(makespan^2) per line: -122675 soft");
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 2 && entries[0].unit_costs == entries[1].unit_costs &&
                      entries[0].unit_costs == std::vector<double>{122500, 0, 0} &&
                      entries[0].scoped && entries[0].units == std::vector<int>{0, 1} &&
                      entries[1].weight == 0.5 && entries[1].exponent == 0.5 &&
                      entries[1].default_weight == 1.0,
              "two instances, the same raw costs over the two lines, each with "
              "its own knobs");
    }
    {
        PackagingSchedule s = repeatSchedule();
        check(levelsAre(
                      levelsWith(
                              copyOf("squaredMakespanCost").offset("squaredMakespanCost", 22500.0),
                              s),
                      0, 0.0, -100000.0),
              "threshold offset 22500: 122500 - 22500 on line 0");
        check(levelsAre(levelsWith(copyOf("squaredMakespanCost")
                                           .offset("squaredMakespanCost", 22500.0)
                                           .clip("squaredMakespanCost", false),
                                   s),
                        0, 0.0, -77500.0),
              "literal offset 22500: the empty line is rewarded, the pool is not");
        check(levelsAre(
                      levelsWith(copyOf("squaredMakespanCost").offset("squaredMakespanCost", -1.0),
                                 s),
                      0, 0.0, -122501.0),
              "threshold offset -1 adds to the non-zero line only");
        check(levelsAre(levelsWith(copyOf("maxEndTimeCost")
                                           .soft("maxEndTimeCost")
                                           .weight("maxEndTimeCost", 3.0)
                                           .offset("maxEndTimeCost", 0.5)
                                           .clip("maxEndTimeCost", false),
                                   s),
                        0, 0.0, 3.0),
              "max end literal offset 0.5 at weight 3 on soft: 2 lines x (-1.5) "
              "-> +3 soft, not the pool");
        check(levelsAre(levelsWith(copyOf("afterIdealEndTimeCost")
                                           .offset("afterIdealEndTimeCost", 40.0)
                                           .clip("afterIdealEndTimeCost", false),
                                   s),
                        0, -20.0, 0.0),
              "after-ideal literal offset 40: (100 - 40) + (0 - 40) medium");
        check(levelsAre(levelsWith(copyOf("unassignedJobsCost")
                                           .offset("unassignedJobsCost", 20.0)
                                           .exponent("unassignedJobsCost", 2.0),
                                   s),
                        0, -900.0, 0.0),
              "unassigned threshold offset 20, exponent 2 on the total: (50 - 20)^2");
        check(levelsAre(levelsWith(copyOf("operatorCleaningConflictCost")
                                           .soft("operatorCleaningConflictCost")
                                           .offset("operatorCleaningConflictCost", 5.0)
                                           .clip("operatorCleaningConflictCost", false),
                                   s),
                        0, 0.0, 5.0),
              "cleaning-conflict literal offset 5 on the single total, on soft: +5 soft");
    }
    {
        // On hard (the default level of maxEndTimeCost and
        // operatorCleaningConflictCost) the literal formula with an offset is
        // refused at reset, whichever sign and setter order: +0.5 would
        // reward both lines (+4 hard, a feasible-looking score), -0.5 would
        // charge lines that end in time.
        auto resetRefusal = [](ConstraintAssembly assembly) {
            PackagingSchedule s = repeatSchedule();
            try {
                levelsWith(std::move(assembly), s);
            } catch (const std::logic_error& error) {
                return std::string(error.what());
            }
            return std::string();
        };
        auto refusal = [](const std::string& name) {
            return "Constraint '" + name +
                   "' cannot use clip=false with a non-zero offset on a level that counts "
                   "toward the hard score (hard 0 must mean no violation)";
        };
        check(resetRefusal(copyOf("maxEndTimeCost")
                                   .offset("maxEndTimeCost", 0.5)
                                   .clip("maxEndTimeCost", false)) == refusal("maxEndTimeCost") &&
                      resetRefusal(copyOf("maxEndTimeCost")
                                           .clip("maxEndTimeCost", false)
                                           .offset("maxEndTimeCost", -0.5)) ==
                              refusal("maxEndTimeCost"),
              "max end literal offset +-0.5 on hard: refused at reset");
        check(resetRefusal(copyOf("operatorCleaningConflictCost")
                                   .offset("operatorCleaningConflictCost", 5.0)
                                   .clip("operatorCleaningConflictCost", false)) ==
                      refusal("operatorCleaningConflictCost"),
              "cleaning-conflict literal offset on its default hard level: refused at reset");
        check(resetRefusal(copyOf("squaredMakespanCost")
                                   .offset("squaredMakespanCost", 22500.0)
                                   .clip("squaredMakespanCost", false)
                                   .hard("squaredMakespanCost")) ==
                              refusal("squaredMakespanCost") &&
                      resetRefusal(copyOf("afterIdealEndTimeCost")
                                           .hard("afterIdealEndTimeCost")
                                           .offset("afterIdealEndTimeCost", 40.0)
                                           .clip("afterIdealEndTimeCost", false)) ==
                              refusal("afterIdealEndTimeCost"),
              "a soft / medium literal pick moved to hard (before or after): refused at reset");
        check(resetRefusal(copyOf("maxEndTimeCost").offset("maxEndTimeCost", 0.5)).empty() &&
                      resetRefusal(copyOf("maxEndTimeCost").clip("maxEndTimeCost", false))
                              .empty() &&
                      resetRefusal(copyOf("afterIdealEndTimeCost")
                                           .offset("afterIdealEndTimeCost", 40.0)
                                           .clip("afterIdealEndTimeCost", false))
                              .empty(),
              "allowed: a hard threshold offset, hard clip=false with offset 0, and the "
              "literal formula on the into_soft medium");
    }
    {
        // Random sequence walk: incremental == a fresh calculator of the same
        // assembly.
        PackagingSchedule s = generateDemoData(/*lines*/ 4, /*jobs*/ 24);
        ScoreCalculator::prepare(s);
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        optimaru::Editor<PackagingSchedule> editor(s, calculator);
        using Descriptor = optimaru::SolutionTraits<PackagingSchedule>;
        const int list_count = Descriptor::list_count(s);
        const int element_count = Descriptor::element_count(s);
        std::vector<int> list_of(element_count, -1);
        for (int l = 0; l < list_count; ++l) {
            for (int p = 0; p < Descriptor::list_size(s, l); ++p)
                list_of[Descriptor::element_at(s, l, p)] = l;
        }
        auto indexWithin = [&](int list, int element) {
            for (int p = 0; p < Descriptor::list_size(s, list); ++p) {
                if (Descriptor::element_at(s, list, p) == element) return p;
            }
            return -1;
        };
        std::mt19937_64 rng(321);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        int mismatches = 0;
        int mediumSteps = 0;
        double drift = 0.0;
        const int steps = 1500;
        for (int step = 0; step < steps && element_count > 0; ++step) {
            const int element = static_cast<int>(rng() % element_count);
            if (unit(rng) < 0.8) {
                const int from = list_of[element];
                if (from >= 0) editor.remove(from, indexWithin(from, element));
                const int to = static_cast<int>(rng() % list_count);
                const int position = static_cast<int>(
                        rng() % static_cast<unsigned>(Descriptor::list_size(s, to) + 1));
                editor.insert(to, position, element);
                list_of[element] = to;
            } else {
                const int list = static_cast<int>(rng() % list_count);
                const int size = Descriptor::list_size(s, list);
                if (size < 2) continue;
                int first = static_cast<int>(rng() % size);
                int last = static_cast<int>(rng() % size);
                if (first > last) std::swap(first, last);
                editor.reverse(list, first, last);
            }
            const optimaru::Score incremental = editor.score();
            PackagingSchedule shadow = s;
            DecomposedCalculator fresh;
            repeatedAssembly().into(fresh);
            fresh.rebuild(shadow);
            const optimaru::Score full = fresh.score();
            if (fresh.level_scores().medium != 0.0) ++mediumSteps;
            drift = std::max(drift, std::fabs(full.soft - incremental.soft));
            if (!optimaru::scores_agree(incremental, full)) ++mismatches;
        }
        std::printf("    largest soft drift %.3g\n", drift);
        check(mismatches == 0, std::to_string(steps) +
                                       " random list moves with repeated, shifted components: "
                                       "incremental == fresh");
        check(mediumSteps > 0, "the walk visits medium states");
    }
    {
        PackagingSchedule s = generateDemoData(/*lines*/ 3, /*jobs*/ 12);
        Solver::Config config;
        config.time_limit = std::chrono::seconds(2);
        config.seed = 5;
        config.score_checks = optimaru::ScoreChecks::Full;
        config.ruinRecreateUnimprovedStepTrigger = 200;
        config.ruinRecreatePolishSteps = 100;
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        bool corrupted = false;
        try {
            Solver solver(config);
            solver.solve(s, calculator);
        } catch (const optimaru::ScoreMismatchError& e) {
            corrupted = true;
            std::printf("  ScoreCorruptionError: %s\n", e.what());
        }
        check(!corrupted, "2s FULL_ASSERT solve with repeated, shifted components");
        DecomposedCalculator fresh;
        repeatedAssembly().into(fresh);
        check(!corrupted && sameScore(s.score, scoreWith(fresh, s)),
              "the reported score == a fresh recompute of the same assembly");
    }
    std::printf("\n");
}

}  // namespace

int main() {
    verifyAssemblyParity();
    verifyIncremental();
    verifyConstraintTotals();
    verifyOverrides();
    verifyFullAssertSolve();
    verifyRawCostsByHand();
    verifyCatalog();
    verifyMediumLevel();
    verifyMediumIncremental();
    verifyPluginCallbackLevels();
    verifyRepeatsAndOffsets();

    if (failures == 0) {
        std::printf("All food-packaging plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d food-packaging plugin check(s) FAILED.\n", failures);
    return 1;
}

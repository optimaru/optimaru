// fab_plugin_test — the plugin assembly (fab/constraints.hpp):
//   (a) the default assembly == the monolithic score bit-for-bit, in data
//       mode and in search mode, on 300 random states
//   (b) plugin incremental == full recompute, per constraint, on random
//       Editor walks in which every constraint becomes non-zero
//   (c) each component alone reports exactly its term
//   (d) weight / level / exponent overrides match hand arithmetic
//   (e) constraint_totals() explains exactly the reported score
//   (f) solve(schedule, calculator) under ScoreChecks::Full
//   (g) raw costs by hand through constraint_costs(): a positive and a zero
//       case per constraint
//   (h) the catalog: info() == the components' kDefaultWeight / kDefaultLevel
//   (i) repeated components and the offset / clip knobs: a hand-calculated
//       repeat, a repeat on its own level (hard next to the soft default),
//       threshold and literal offsets by hand on the totals (a raw 0 counts
//       in literal mode), the literal formula refused on hard at reset and
//       kept on soft, incremental == a fresh recompute over random walks
//       (every repeat reads the calculator's one shared schedule view) and
//       ScoreChecks::Full solves with repeated, shifted components on both levels
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "fab/constraints.hpp"
#include "fab/score_calculator.hpp"
#include "fab/solver.hpp"
#include "fab_fixtures.hpp"
#include "optimaru/model/editor.hpp"

using namespace fab;
using namespace fabtest;

namespace {

Score scoreWith(DecomposedCalculator& calculator, SemiconductorSchedule& schedule) {
    calculator.rebuild(schedule);
    return calculator.score();
}

// Exact equality of both levels (the hand cases below are exact in binary).
bool sameScore(const Score& a, const Score& b) { return a.hard == b.hard && a.soft == b.soft; }

// Raw cost per constraint name from constraint_costs() (one entry per global).
std::map<std::string, double> rawByName(SemiconductorSchedule& schedule) {
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.rebuild(schedule);
    calculator.score();
    std::map<std::string, double> out;
    for (const auto& entry : calculator.constraint_costs())
        out[entry.name] = entry.unit_costs.size() == 1 ? entry.unit_costs[0] : -1.0;
    return out;
}

void randomizeValues(SemiconductorSchedule& s, std::mt19937_64& rng) {
    for (OperationRow& row : s.rows) row.value = randomValue(s, rng, true);
}

// (a)
void verifyAssemblyParity() {
    int mismatches = 0;
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    std::mt19937_64 rng(5);
    for (int round = 0; round < 300; ++round) {
        SemiconductorSchedule s = randomInstance(static_cast<std::uint64_t>(round) + 17);
        // data mode
        const Score data = scoreWith(calculator, s);
        SemiconductorSchedule copy = s;
        if (!(data == ScoreCalculator::score(copy))) ++mismatches;
        // search mode, at a random state (seed or not)
        s.warmStart = round % 2 == 0;
        beginSearch(s);
        if (round % 3 != 0) randomizeValues(s, rng);
        if (!(scoreWith(calculator, s) == ScoreCalculator::searchStateScore(s))) ++mismatches;
    }
    check(mismatches == 0,
          "(a) default assembly == monolith on 300 random states (data + search mode)");
}

// The raw cost of every constraint of `calculator` (one global each), in
// Term order; a constraint the assembly lacks reads -1.
RawTotals rawOfAssembly(const DecomposedCalculator& calculator) {
    RawTotals out;
    out.fill(-1);
    for (const auto& entry : calculator.constraint_costs()) {
        for (int t = 0; t < kTermCount; ++t) {
            if (entry.name == kTermNames[t] && entry.unit_costs.size() == 1)
                out[static_cast<std::size_t>(t)] = static_cast<std::int64_t>(entry.unit_costs[0]);
        }
    }
    return out;
}

// (b)
void verifyIncremental() {
    int mismatches = 0;
    TermHits hits;
    for (int instance = 0; instance < 60; ++instance) {
        SemiconductorSchedule s = randomInstance(static_cast<std::uint64_t>(instance) * 31 + 3);
        s.warmStart = instance % 2 == 0;
        beginSearch(s);
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        optimaru::Editor<SemiconductorSchedule> editor(s, calculator);
        std::mt19937_64 rng(static_cast<std::uint64_t>(instance));
        const int n = s.rowCount();
        for (int m = 0; m < 300; ++m) {
            const int row = static_cast<int>(rng() % static_cast<std::uint64_t>(n));
            const int old = s.rows[static_cast<std::size_t>(row)].value;
            if (m % 4 == 3 && n > 1) {
                const int other = static_cast<int>(rng() % static_cast<std::uint64_t>(n));
                editor.begin_batch();
                editor.assign(row, randomValue(s, rng, false));
                editor.assign(other, randomValue(s, rng, true));
                editor.end_batch();
            } else {
                editor.assign(row, randomValue(s, rng, true));
                if (m % 5 == 0) {
                    editor.score();
                    editor.assign(row, old);  // a rejected move
                }
            }
            // Every constraint's raw cost, then the score.
            const Score score = editor.score();
            const RawTotals full = ScoreCalculator::searchStateRawCosts(s);
            if (!(rawOfAssembly(calculator) == full &&
                  score == ScoreCalculator::searchStateScore(s)))
                ++mismatches;
            if (!atSeed(s)) hits.add(full);
        }
    }
    check(mismatches == 0,
          "(b) plugin incremental == full recompute, per constraint, on 60 x 300 moves");
    check(hits.missing().empty(),
          "(b) every constraint is non-zero in some decoded state of the walk (never: " +
                  hits.missing() + ")");
    std::printf("  (b) the rarest term is non-zero in %lld decoded states\n",
                static_cast<long long>(hits.fewest()));
}

// (c)
void verifyComponentsAlone() {
    int mismatches = 0;
    for (int round = 0; round < 40; ++round) {
        SemiconductorSchedule s = randomInstance(static_cast<std::uint64_t>(round) + 500);
        const RawTotals raw = ScoreCalculator::rawCosts(s);
        for (int t = 0; t < kTermCount; ++t) {
            DecomposedCalculator calculator;
            defaultConstraints().only({kTermNames[t]}).into(calculator);
            const Score score = scoreWith(calculator, s);
            const std::int64_t expected = raw[static_cast<std::size_t>(t)];
            const double weight = t == static_cast<int>(Term::WeightedTardinessCost) ? 10.0 : 1.0;
            const bool ok = t < kHardTermCount
                                    ? score.hard == -expected && score.soft == 0.0
                                    : score.hard == 0 && score.soft == -weight * expected;
            if (!ok) ++mismatches;
        }
    }
    check(mismatches == 0, "(c) each component alone scores exactly its term");
}

// tests/test_semiconductor_scheduling.py::_fab_problem with its assignments.
SemiconductorSchedule fabProblem() {
    Builder b;
    b.machine("M1", "PHOTO", 1, 1, {{3, 5}}).machine("M2", "PHOTO").machine("F1", "DIFF", 2, 2);
    b.setup("PHOTO", "X", "Y", 3).transport("PHOTO", "DIFF", 3);
    b.lot("L1", 2, 12, 2).op("PHOTO", "X", {{"M1", 4, 1}}, {}, 0, 3).op("DIFF", "X", {{"F1", 5}});
    b.lot("L2", 0, 30).op("PHOTO", "Y", {{"M1", 4}}).op("DIFF", "Y", {{"F1", 5}});
    b.lot("L3", 0, 30).op("DIFF", "X", {{"F1", 5}});
    b.assign("L1", "PHOTO", "M1", 0, 4).assign("L1", "DIFF", "F1", 10, 15);
    b.assign("L2", "PHOTO", "M1", 2, 6).assign("L2", "DIFF", "F1", 11, 16);
    b.assign("L3", "DIFF", "F1", 12, 17);
    return b.build();
}

// (d)
void verifyOverrides() {
    SemiconductorSchedule s = fabProblem();
    // hard 26, soft 135 at the defaults (fab_test).
    {
        DecomposedCalculator calculator;
        defaultConstraints().soft("machineCapacityCost").into(calculator);
        const Score score = scoreWith(calculator, s);
        check(score.hard == -24 && score.soft == -137.0,
              "(d) machineCapacityCost demoted: 2 moves to soft");
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints()
                .weight("weightedTardinessCost", 4)
                .hard("makespanCost")
                .into(calculator);
        const Score score = scoreWith(calculator, s);
        check(score.hard == -(26 + 17) && score.soft == -(1.0 + 3 + 8 + 4 * 6 + 46),
              "(d) tardiness weight 4, makespan made hard");
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints()
                .exponent("batchFamilyConflictCost", 2.0)
                .exponent("cycleTimeCost", 0.5)
                .into(calculator);
        const Score score = scoreWith(calculator, s);
        check(score.hard == -(26 - 8 + 64),
              "(d) exponent 2 shapes the family conflict TOTAL: 8 -> 64");
        check(std::fabs(score.soft - (-(1.0 + 3 + 8 + 60 + std::sqrt(46.0) + 17))) < 1e-9,
              "(d) exponent 0.5 shapes the cycle-time total");
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints().weight("maxQueueTimeCost", 0.5).into(calculator);
        // 0.5 * 3 = 1.5 rounds half away from zero to 2, per constraint.
        check(scoreWith(calculator, s).hard == -(26 - 3 + 2),
              "(d) fractional hard weight rounds per constraint");
    }
}

// (e)
void verifyTotals() {
    SemiconductorSchedule s = fabProblem();
    DecomposedCalculator calculator;
    defaultConstraints().weight("makespanCost", 3).soft("lotRoutePrecedenceCost").into(calculator);
    const Score score = scoreWith(calculator, s);
    double hard = 0;
    double soft = 0;
    for (const auto& total : calculator.constraint_totals())
        (total.is_hard ? hard : soft) += total.cost;
    check(score.hard == -static_cast<std::int64_t>(hard) && score.soft == -soft,
          "(e) constraintTotals() sum to the score");
}

// (f)
void verifySolveFullAssert() {
    bool ok = true;
    try {
        for (int seed = 0; seed < 4; ++seed) {
            SemiconductorSchedule s = randomInstance(900 + static_cast<std::uint64_t>(seed));
            s.warmStart = seed % 2 == 1;
            DecomposedCalculator calculator;
            defaultConstraints().weight("weightedTardinessCost", 2).into(calculator);
            Solver::Config config;
            config.score_checks = optimaru::ScoreChecks::Full;
            config.step_limit = 300;
            config.seed = static_cast<std::uint64_t>(seed);
            Solver(config).solve(s, calculator);
            DecomposedCalculator again;
            defaultConstraints().weight("weightedTardinessCost", 2).into(again);
            ok &= scoreWith(again, s) == s.score && !s.searching;
        }
    } catch (const std::exception& error) {
        std::printf("  %s\n", error.what());
        ok = false;
    }
    check(ok, "(f) solve(schedule, calculator) under FULL_ASSERT; the score is the assembly's");
}

// (g) raw costs by hand, through constraint_costs().
void verifyRawCostsByHand() {
    SemiconductorSchedule s = fabProblem();
    auto raw = rawByName(s);
    const std::map<std::string, double> expected{
            {"machineEligibilityCost", 0},
            {"lotRoutePrecedenceCost", 2},
            {"machineCapacityCost", 2},
            {"batchCapacityCost", 3},
            {"batchFamilyConflictCost", 8},
            {"missingSetupTimeCost", 5},
            {"machineUnavailabilityCost", 3},
            {"maxQueueTimeCost", 3},
            {"minQueueTimeCost", 0},
            {"missingTransportTimeCost", 0},
            {"auxiliaryResourceCapacityCost", 0},
            {"auxiliaryResourceUnavailabilityCost", 0},
            {"pinnedAssignmentsCost", 0},
            {"maxCycleTimeCost", 0},
            {"machinePreferenceCost", 1},
            {"minBatchSizeCost", 3},
            {"machineLoadImbalanceCost", 8},
            {"weightedTardinessCost", 6},
            {"cycleTimeCost", 46},
            {"makespanCost", 17},
    };
    check(raw == expected, "(g) fab fixture: every raw cost by hand");

    Builder r;
    r.machine("M1", "G1").machine("M2", "G2").resource("RETICLE", 1, {{3, 5}});
    r.lot("L1", 0, 20, 1, 6).op("O1", "F", {{"M1", 4}}, {"RETICLE"}, 4).op("O2", "F", {{"M2", 2}});
    r.lot("L2", 0, 20).op("O1", "F", {{"M1", 4}}, {"RETICLE"});
    r.pin("L1", "O1", "M1", 1, 5);
    r.assign("L1", "O1", "M1", 0, 4).assign("L1", "O2", "M2", 6, 8).assign("L2", "O1", "M1", 2, 6);
    SemiconductorSchedule rs = r.build();
    raw = rawByName(rs);
    check(raw["minQueueTimeCost"] == 2 && raw["auxiliaryResourceCapacityCost"] == 2 &&
                  raw["auxiliaryResourceUnavailabilityCost"] == 3 &&
                  raw["pinnedAssignmentsCost"] == 1 && raw["maxCycleTimeCost"] == 2 &&
                  raw["machineEligibilityCost"] == 0,
          "(g) resource fixture: min queue 2, resource 2 and 3, pin 1, max cycle 2");

    // A feasible schedule: every term but cycle time and makespan is 0.
    Builder z;
    z.machine("M1", "G1", 1, 1, {{20, 25}}).machine("M2", "G1").machine("F1", "G2", 2, 2);
    z.resource("R", 1, {{30, 35}});
    z.setup("G1", "A", "B", 2).transport("G1", "G2", 1);
    z.lot("L1", 0, 50, 1, 40).op("O1", "A", {{"M1", 4}}, {"R"}, 1, 10).op("O2", "X", {{"F1", 5}});
    z.lot("L2", 0, 50).op("O1", "B", {{"M1", 3}}).op("O2", "X", {{"F1", 5}});
    z.lot("L3", 0, 50).op("O1", "B", {{"M2", 7}});
    z.pin("L1", "O1", "M1", 0, 4);
    z.assign("L1", "O1", "M1", 0, 4).assign("L2", "O1", "M1", 6, 9);
    z.assign("L1", "O2", "F1", 10, 15, "B1").assign("L2", "O2", "F1", 10, 15, "B1");
    z.assign("L3", "O1", "M2", 0, 7);
    SemiconductorSchedule zs = z.build();
    raw = rawByName(zs);
    bool zero = true;
    for (const auto& [name, value] : raw) {
        if (name != "cycleTimeCost" && name != "makespanCost") zero &= value == 0.0;
    }
    check(zero && raw["cycleTimeCost"] == 37 && raw["makespanCost"] == 15,
          "(g) feasible fixture: 18 terms 0, cycle 37, makespan 15");
    zs.assignments.clear();
    raw = rawByName(zs);
    check(raw["cycleTimeCost"] == 0 && raw["makespanCost"] == 0 &&
                  raw["weightedTardinessCost"] == 0,
          "(g) nothing completed: cycle time, makespan, tardiness 0");
}

// (h)
template <typename Component>
bool rowIs(const optimaru::ConstraintInfo& row, const char* name, optimaru::Level level,
           double weight) {
    return row.name == name && row.default_level == Component::kDefaultLevel &&
           row.default_weight == Component::kDefaultWeight && row.default_level == level &&
           row.default_weight == weight && row.level_flippable && !row.reward && row.is_global &&
           row.default_enabled;
}

void verifyCatalog() {
    using L = optimaru::Level;
    const auto rows = defaultConstraints().info();
    bool ok = rows.size() == 20;
    ok = ok && rowIs<MachineEligibilityCost>(rows[0], "machineEligibilityCost", L::Hard, 1.0) &&
         rowIs<LotRoutePrecedenceCost>(rows[1], "lotRoutePrecedenceCost", L::Hard, 1.0) &&
         rowIs<MachineCapacityCost>(rows[2], "machineCapacityCost", L::Hard, 1.0) &&
         rowIs<BatchCapacityCost>(rows[3], "batchCapacityCost", L::Hard, 1.0) &&
         rowIs<BatchFamilyConflictCost>(rows[4], "batchFamilyConflictCost", L::Hard, 1.0) &&
         rowIs<MissingSetupTimeCost>(rows[5], "missingSetupTimeCost", L::Hard, 1.0) &&
         rowIs<MachineUnavailabilityCost>(rows[6], "machineUnavailabilityCost", L::Hard, 1.0) &&
         rowIs<MaxQueueTimeCost>(rows[7], "maxQueueTimeCost", L::Hard, 1.0) &&
         rowIs<MinQueueTimeCost>(rows[8], "minQueueTimeCost", L::Hard, 1.0) &&
         rowIs<MissingTransportTimeCost>(rows[9], "missingTransportTimeCost", L::Hard, 1.0) &&
         rowIs<AuxiliaryResourceCapacityCost>(rows[10], "auxiliaryResourceCapacityCost", L::Hard,
                                              1.0) &&
         rowIs<AuxiliaryResourceUnavailabilityCost>(rows[11], "auxiliaryResourceUnavailabilityCost",
                                                    L::Hard, 1.0) &&
         rowIs<PinnedAssignmentsCost>(rows[12], "pinnedAssignmentsCost", L::Hard, 1.0) &&
         rowIs<MaxCycleTimeCost>(rows[13], "maxCycleTimeCost", L::Hard, 1.0) &&
         rowIs<MachinePreferenceCost>(rows[14], "machinePreferenceCost", L::Soft, 1.0) &&
         rowIs<MinBatchSizeCost>(rows[15], "minBatchSizeCost", L::Soft, 1.0) &&
         rowIs<MachineLoadImbalanceCost>(rows[16], "machineLoadImbalanceCost", L::Soft, 1.0) &&
         rowIs<WeightedTardinessCost>(rows[17], "weightedTardinessCost", L::Soft, 10.0) &&
         rowIs<CycleTimeCost>(rows[18], "cycleTimeCost", L::Soft, 1.0) &&
         rowIs<MakespanCost>(rows[19], "makespanCost", L::Soft, 1.0);
    check(ok, "(h) info() == the components' constants, in list order");
    bool registryOrder = true;
    for (int t = 0; t < kTermCount; ++t)
        registryOrder &= rows[static_cast<std::size_t>(t)].name == kTermNames[t];
    check(registryOrder, "(h) catalog order == Term order");
    // Every constraint flips both ways.
    bool flips = true;
    try {
        defaultConstraints().soft("pinnedAssignmentsCost").hard("makespanCost");
    } catch (const std::invalid_argument&) {
        flips = false;
    }
    check(flips, "(h) hard constraints demote, soft ones promote");
}

// (i) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with add_instances(), as the Python bindings build
// it from a pick list. Every component is global: the offset shapes the
// instance's total. The copies share the calculator's one "fab.schedule"
// view, which resets once per calculator reset and decodes once per score.
ConstraintAssembly copyOf(const std::string& name) {
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

Score scoreOf(ConstraintAssembly assembly, SemiconductorSchedule s) {
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    return scoreWith(calculator, s);
}

// Every constraint stays in; hard and soft totals repeated or shifted in
// both modes, a fractional hard weight on a threshold offset, a soft one made
// hard and back. Hard refuses the literal formula with an offset, so the
// literal copies of hard defaults sit on soft, next to a hard threshold copy
// with the same knobs.
ConstraintAssembly repeatedAssembly() {
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("makespanCost", 5.0).clip("makespanCost", false);
    assembly.offset("machineCapacityCost", 1.0);
    assembly.add_instances(std::move(copyOf("makespanCost")
                                             .weight("makespanCost", 0.5)
                                             .exponent("makespanCost", 2.0)
                                             .offset("makespanCost", 10.0)));
    assembly.add_instances(std::move(copyOf("lotRoutePrecedenceCost")
                                             .weight("lotRoutePrecedenceCost", 1.5)
                                             .offset("lotRoutePrecedenceCost", 0.5)));
    assembly.add_instances(std::move(copyOf("lotRoutePrecedenceCost")
                                             .soft("lotRoutePrecedenceCost")
                                             .weight("lotRoutePrecedenceCost", 1.5)
                                             .offset("lotRoutePrecedenceCost", 0.5)
                                             .clip("lotRoutePrecedenceCost", false)));
    assembly.add_instances(
            std::move(copyOf("batchFamilyConflictCost").offset("batchFamilyConflictCost", -2.0)));
    assembly.add_instances(
            std::move(copyOf("pinnedAssignmentsCost").offset("pinnedAssignmentsCost", -1.0)));
    assembly.add_instances(std::move(copyOf("pinnedAssignmentsCost")
                                             .soft("pinnedAssignmentsCost")
                                             .offset("pinnedAssignmentsCost", -1.0)
                                             .clip("pinnedAssignmentsCost", false)));
    assembly.add_instances(std::move(copyOf("weightedTardinessCost")
                                             .weight("weightedTardinessCost", 0.25)
                                             .exponent("weightedTardinessCost", 1.5)
                                             .offset("weightedTardinessCost", 20.0)
                                             .clip("weightedTardinessCost", false)));
    assembly.add_instances(
            std::move(copyOf("cycleTimeCost").hard("cycleTimeCost").offset("cycleTimeCost", 30.0)));
    assembly.add_instances(std::move(copyOf("machineCapacityCost").soft("machineCapacityCost")));
    return assembly;
}

void verifyRepeatsAndOffsets() {
    // fabProblem(): makespan 17, cycle time 46, machine capacity 2, family
    // conflict 8, max queue time 3, no pins (pinnedAssignmentsCost 0).
    const SemiconductorSchedule s = fabProblem();
    {
        ConstraintAssembly assembly = copyOf("makespanCost");
        assembly.add_instances(std::move(
                copyOf("makespanCost").weight("makespanCost", 0.5).exponent("makespanCost", 2.0)));
        DecomposedCalculator calculator;
        assembly.into(calculator);
        SemiconductorSchedule copy = s;
        const Score score = scoreWith(calculator, copy);
        check(score.hard == 0 && score.soft == -(17 + 0.5 * 289),
              "(i) makespan + 0.5 x makespan^2: -161.5 soft");
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 2 && entries[0].unit_costs == entries[1].unit_costs &&
                      entries[0].unit_costs == std::vector<double>{17.0} &&
                      entries[1].weight == 0.5 && entries[1].exponent == 2.0 &&
                      entries[1].default_weight == 1.0 && entries[0].name == entries[1].name,
              "(i) two instances, the same raw total, each with its own knobs");
        ConstraintAssembly twice = copyOf("machineCapacityCost");
        twice.add_instances(copyOf("machineCapacityCost"));
        check(scoreOf(std::move(twice), s).hard == -4, "(i) identical picks count twice: 2 x 2");
    }
    {
        // Each instance on its own level: the soft makespan 17 next to a hard
        // repeat, 2 x (17 - 15) beyond its offset.
        ConstraintAssembly assembly = copyOf("makespanCost");
        assembly.add_instances(std::move(copyOf("makespanCost")
                                                 .hard("makespanCost")
                                                 .weight("makespanCost", 2.0)
                                                 .offset("makespanCost", 15.0)));
        DecomposedCalculator calculator;
        assembly.into(calculator);
        SemiconductorSchedule copy = s;
        const Score score = scoreWith(calculator, copy);
        check(score.hard == -4 && score.soft == -17.0,
              "(i) soft makespan + a hard repeat beyond 15: -4 hard / -17 soft");
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 2 && entries[0].level == optimaru::Level::Soft &&
                      entries[1].level == optimaru::Level::Hard &&
                      entries[1].default_level == optimaru::Level::Soft,
              "(i) the repeat reports its own level, the default stays soft");
    }
    check(scoreOf(copyOf("makespanCost").offset("makespanCost", 10.0), s).soft == -7.0,
          "(i) threshold offset 10 on the makespan 17: -7");
    check(scoreOf(copyOf("makespanCost").offset("makespanCost", 20.0).clip("makespanCost", false),
                  s)
                          .soft == 3.0,
          "(i) literal offset 20 on 17: 17 - 20 = -3, +3 soft");
    // The hard defaults refuse the literal formula with an offset, so its
    // numbers on them are checked on soft (not rounded).
    check(sameScore(scoreOf(copyOf("pinnedAssignmentsCost")
                                    .soft("pinnedAssignmentsCost")
                                    .offset("pinnedAssignmentsCost", 2.0)
                                    .clip("pinnedAssignmentsCost", false),
                            s),
                    Score{0, 2.0}),
          "(i) soft literal offset 2 on a raw 0 (no pins): charged, +2");
    check(scoreOf(copyOf("pinnedAssignmentsCost").offset("pinnedAssignmentsCost", -1.0), s).hard ==
                  0,
          "(i) a negative threshold offset leaves a raw 0 at 0");
    check(sameScore(scoreOf(copyOf("pinnedAssignmentsCost")
                                    .soft("pinnedAssignmentsCost")
                                    .offset("pinnedAssignmentsCost", -1.0)
                                    .clip("pinnedAssignmentsCost", false),
                            s),
                    Score{0, -1.0}),
          "(i) a negative soft literal offset charges a raw 0: 0 + 1");
    check(scoreOf(copyOf("batchFamilyConflictCost").offset("batchFamilyConflictCost", -2.0), s)
                          .hard == -10,
          "(i) a negative threshold offset adds to a non-zero total: 8 + 2");
    check(sameScore(scoreOf(copyOf("maxQueueTimeCost").offset("maxQueueTimeCost", 0.5), s),
                    Score{-3, 0.0}),
          "(i) a hard threshold offset rounds the total: 3 - 0.5 = 2.5 -> 3");
    check(sameScore(scoreOf(copyOf("maxQueueTimeCost")
                                    .soft("maxQueueTimeCost")
                                    .offset("maxQueueTimeCost", 0.5)
                                    .clip("maxQueueTimeCost", false),
                            s),
                    Score{0, -2.5}),
          "(i) a soft literal offset is not rounded: 3 - 0.5 = 2.5");
    check(sameScore(scoreOf(copyOf("pinnedAssignmentsCost")
                                    .soft("pinnedAssignmentsCost")
                                    .weight("pinnedAssignmentsCost", 3.0)
                                    .offset("pinnedAssignmentsCost", 0.5)
                                    .clip("pinnedAssignmentsCost", false),
                            s),
                    Score{0, 1.5}),
          "(i) soft: 3 x (0 - 0.5) = -1.5, +1.5");
    {
        DecomposedCalculator calculator;
        copyOf("cycleTimeCost")
                .offset("cycleTimeCost", 2.5)
                .clip("cycleTimeCost", false)
                .into(calculator);
        SemiconductorSchedule copy = s;
        scoreWith(calculator, copy);
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 1 && entries[0].offset == 2.5 && !entries[0].clip &&
                      !entries[0].scoped && entries[0].unit_costs == std::vector<double>{46.0},
              "(i) constraintCosts() reports the offset and clip, the raw total unshaped");
    }
    {
        // On hard the literal formula with an offset is refused at reset,
        // whichever sign and setter order, for a hard default and for the
        // soft makespan moved to hard: +2 on the machine capacity total 2
        // would score it as 0 (and +8 would turn the family conflicts' 8 into
        // a hard 0), -1 would charge a total without a violation.
        auto refusal = [](const std::string& name) {
            return "Constraint '" + name +
                   "' cannot use clip=false with a non-zero offset on a level that counts "
                   "toward the hard score (hard 0 must mean no violation)";
        };
        auto resetRefusal = [&s](ConstraintAssembly assembly) {
            try {
                scoreOf(std::move(assembly), s);
            } catch (const std::logic_error& error) {
                return std::string(error.what());
            }
            return std::string();
        };
        check(resetRefusal(copyOf("machineCapacityCost")
                                   .offset("machineCapacityCost", 2.0)
                                   .clip("machineCapacityCost", false)) ==
                              refusal("machineCapacityCost") &&
                      resetRefusal(copyOf("machineCapacityCost")
                                           .clip("machineCapacityCost", false)
                                           .offset("machineCapacityCost", -1.0)) ==
                              refusal("machineCapacityCost"),
              "(i) machine capacity literal offset +2 / -1 on its default hard: refused at reset");
        check(resetRefusal(copyOf("makespanCost")
                                   .offset("makespanCost", 20.0)
                                   .clip("makespanCost", false)
                                   .hard("makespanCost")) == refusal("makespanCost"),
              "(i) makespan literal offset moved to hard afterwards: refused at reset");
        ConstraintAssembly repeated = copyOf("lotRoutePrecedenceCost");
        repeated.add_instances(std::move(copyOf("lotRoutePrecedenceCost")
                                                 .offset("lotRoutePrecedenceCost", 1.0)
                                                 .clip("lotRoutePrecedenceCost", false)));
        check(resetRefusal(std::move(repeated)) == refusal("lotRoutePrecedenceCost"),
              "(i) a hard literal repeat next to the default: refused at reset");
        check(resetRefusal(copyOf("machineCapacityCost").offset("machineCapacityCost", 2.0))
                              .empty() &&
                      resetRefusal(copyOf("machineCapacityCost").clip("machineCapacityCost", false))
                              .empty() &&
                      resetRefusal(copyOf("machineCapacityCost")
                                           .offset("machineCapacityCost", 2.0)
                                           .clip("machineCapacityCost", false)
                                           .soft("machineCapacityCost"))
                              .empty(),
              "(i) machine capacity: a threshold offset or clip=false with offset 0 on hard, and "
              "the "
              "literal offset on soft, are allowed");
    }
    {
        int mismatches = 0;
        int moves = 0;
        for (int instance = 0; instance < 30; ++instance) {
            SemiconductorSchedule w =
                    randomInstance(static_cast<std::uint64_t>(instance) * 37 + 11);
            w.warmStart = instance % 2 == 0;
            beginSearch(w);
            DecomposedCalculator calculator;
            repeatedAssembly().into(calculator);
            optimaru::Editor<SemiconductorSchedule> editor(w, calculator);
            std::mt19937_64 rng(static_cast<std::uint64_t>(instance) + 77);
            const int n = w.rowCount();
            for (int m = 0; m < 200; ++m, ++moves) {
                const int row = static_cast<int>(rng() % static_cast<std::uint64_t>(n));
                const int old = w.rows[static_cast<std::size_t>(row)].value;
                if (m % 4 == 3 && n > 1) {
                    const int other = static_cast<int>(rng() % static_cast<std::uint64_t>(n));
                    editor.begin_batch();
                    editor.assign(row, randomValue(w, rng, false));
                    editor.assign(other, randomValue(w, rng, true));
                    editor.end_batch();
                } else {
                    editor.assign(row, randomValue(w, rng, true));
                    if (m % 5 == 0) {
                        editor.score();
                        editor.assign(row, old);  // a rejected move
                    }
                }
                const Score incremental = editor.score();
                SemiconductorSchedule copy = w;
                DecomposedCalculator fresh;
                repeatedAssembly().into(fresh);
                const Score full = scoreWith(fresh, copy);
                if (!optimaru::scores_agree(incremental, full) && ++mismatches <= 3) {
                    std::printf("  MISMATCH instance %d move %d: incremental=%s full=%s\n",
                                instance, m, incremental.str().c_str(), full.str().c_str());
                }
            }
        }
        check(mismatches == 0,
              "(i) " + std::to_string(moves) +
                      " moves with repeated, shifted components: incremental == fresh");
    }
    bool ok = true;
    try {
        for (int seed = 0; seed < 4; ++seed) {
            SemiconductorSchedule w = randomInstance(950 + static_cast<std::uint64_t>(seed));
            w.warmStart = seed % 2 == 1;
            DecomposedCalculator calculator;
            repeatedAssembly().into(calculator);
            Solver::Config config;
            config.score_checks = optimaru::ScoreChecks::Full;
            config.step_limit = 300;
            config.seed = static_cast<std::uint64_t>(seed);
            Solver(config).solve(w, calculator);
            DecomposedCalculator again;
            repeatedAssembly().into(again);
            ok &= optimaru::scores_agree(w.score, scoreWith(again, w)) && !w.searching;
        }
    } catch (const std::exception& error) {
        std::printf("  %s\n", error.what());
        ok = false;
    }
    check(ok,
          "(i) FULL_ASSERT solves with repeated, shifted components; the score is the assembly's");
}

}  // namespace

int main() {
    verifyAssemblyParity();
    verifyIncremental();
    verifyComponentsAlone();
    verifyOverrides();
    verifyTotals();
    verifySolveFullAssert();
    verifyRawCostsByHand();
    verifyCatalog();
    verifyRepeatsAndOffsets();
    return finish("fab_plugin_test");
}

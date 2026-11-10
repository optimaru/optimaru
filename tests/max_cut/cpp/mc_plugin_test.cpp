// Plugin-assembly tests (mc/constraints.hpp), engine test/ style.
//   (a) monolith vs plugin: the default assembly == ScoreCalculator bit for
//       bit on random assignments (-1 sides included), the score and every
//       unit cost (== ScoreCalculator::rawCosts)
//   (b) plugin incremental == full recompute: random moves through the
//       engine's Editor protocol, with the defaults and with the
//       opt-in imbalance switched on; after each move the score and every
//       unit cost equal the full recompute's, and every constraint is
//       non-zero at some point of the walk
//   (c) dropping one component removes exactly its term
//   (d) weight / level / exponent overrides
//   (e) constraint_totals() explains the score
//   (f) a ScoreChecks::Full solve(problem, calculator), exact size included
//   (g) the defaultConstraints() builder: named edits == a by-hand assembly
//   (h) raw costs by hand through constraint_costs(): positive and zero per
//       constraint (fixture H), the edge cases, the -1 rules
//   (i) the catalog (info() == the components' constants) and the guards
//   (j) repeated components and the offset / clip knobs: a hand-calculated
//       repeat, a repeat on its own level (hard next to the soft default),
//       threshold and literal offsets by hand per side (a side of raw 0
//       counts in literal mode) and on the globals' totals, the literal
//       formula refused on hard at reset and kept on soft, incremental == a
//       fresh recompute over a random walk and a ScoreChecks::Full solve with
//       repeated, shifted components on both levels
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
#include <vector>

#include "mc/constraints.hpp"
#include "mc/domain.hpp"
#include "mc/score_calculator.hpp"
#include "mc/solver.hpp"
#include "mc_fixtures.hpp"
#include "optimaru/model/editor.hpp"

using namespace mc;
using namespace mc_test;

namespace {

Score scoreWith(DecomposedCalculator& calculator, MaxCutProblem& problem) {
    ScoreCalculator::prepare(problem);
    calculator.rebuild(problem);
    return calculator.score();
}

std::vector<std::vector<double>> rawTotals(const DecomposedCalculator& calculator) {
    std::vector<std::vector<double>> totals;
    for (const auto& entry : calculator.constraint_costs()) totals.push_back(entry.unit_costs);
    return totals;
}

// The unit costs the default assembly must report for `raw` (the monolith's
// full recompute), in catalog order: [side 0, side 1] for the four unit
// constraints, one total for the imbalance and the cut reward (-cut).
std::vector<std::vector<double>> expectedUnitCosts(const RawCosts& raw) {
    auto sides = [](const std::int64_t (&costs)[kSideCount]) {
        return std::vector<double>{static_cast<double>(costs[0]), static_cast<double>(costs[1])};
    };
    return {sides(raw.fixed),
            sides(raw.same),
            sides(raw.different),
            sides(raw.size),
            {static_cast<double>(raw.imbalance)},
            {-static_cast<double>(raw.cut)}};
}

const char* const kCatalogNames[] = {"fixedPartitionCost",          "samePartitionPairsCost",
                                     "differentPartitionPairsCost", "partitionSizeBoundsCost",
                                     "partitionImbalanceCost",      "cutWeightCost"};

// -------------------------------------------------------------------------
// (a) Monolith vs plugin.
// -------------------------------------------------------------------------
void verifyAssemblyParity() {
    std::printf("== (a) Default assembly == monolithic score ==\n");
    int mismatches = 0;
    int costMismatches = 0;
    const int assignments = 500;
    for (int k = 0; k < assignments; ++k) {
        MaxCutProblem p = randomInstance(1 + k % 25, 1000 + static_cast<std::uint64_t>(k),
                                         k % 3 == 0 ? 0.2 : 0.0);
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        const Score plugin = scoreWith(calculator, p);
        const Score monolith = ScoreCalculator::score(p);
        if (!sameScore(plugin, monolith) && ++mismatches <= 3) {
            std::printf("  MISMATCH #%d: plugin=%s monolith=%s\n", k, plugin.str().c_str(),
                        monolith.str().c_str());
        }
        if (rawTotals(calculator) != expectedUnitCosts(ScoreCalculator::rawCosts(p)) &&
            ++costMismatches <= 3)
            std::printf("  RAW COST MISMATCH #%d\n", k);
    }
    check(mismatches == 0,
          std::to_string(assignments) + " random assignments (-1 included): plugin == monolith");
    check(costMismatches == 0,
          std::to_string(assignments) +
                  " random assignments: every unit cost == ScoreCalculator::rawCosts");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Plugin incremental == full recompute, per constraint.
// -------------------------------------------------------------------------
void verifyIncremental() {
    std::printf("== (b) Plugin incremental == full recompute, per constraint ==\n");
    // Two assemblies: the defaults (compared with the monolith's score) and
    // the opt-in imbalance switched on at weight 1, so its incremental term
    // also reaches the score: hard -(fixed + same + different + size), soft
    // cut - imbalance.
    for (const bool imbalanceOn : {false, true}) {
        int nonZero[6] = {0, 0, 0, 0, 0, 0};
        int mismatches = 0;
        int costMismatches = 0;
        int moves = 0;
        for (std::uint64_t seed = 7; seed < 10; ++seed) {
            MaxCutProblem p = randomInstance(40, seed, 0.1);
            ScoreCalculator::prepare(p);
            DecomposedCalculator calculator;
            if (imbalanceOn)
                defaultConstraints().weight("partitionImbalanceCost", 1).into(calculator);
            else
                addDefaultConstraints(calculator);
            optimaru::Editor<MaxCutProblem> editor(p, calculator);
            MaxCutProblem shadow = p;

            std::mt19937_64 rng(seed * 123);
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            const int n = p.vertexCount();
            for (int step = 0; step < 1000; ++step, ++moves) {
                const double u = unit(rng);
                if (u < 0.6) {
                    const int v = static_cast<int>(rng() % n);
                    const int value = unit(rng) < 0.15 ? kUnassigned : static_cast<int>(rng() % 2);
                    editor.assign(v, value);
                } else {
                    editor.begin_batch();
                    const int a = static_cast<int>(rng() % n);
                    const int b = static_cast<int>(rng() % n);
                    const int va = p.side[a];
                    editor.assign(a, p.side[b]);
                    editor.assign(b, va);
                    if (u > 0.9)
                        editor.assign(static_cast<int>(rng() % n), static_cast<int>(rng() % 2));
                    editor.end_batch();
                }
                shadow.side = p.side;
                Score full = ScoreCalculator::score(shadow);
                const RawCosts raw = ScoreCalculator::rawCosts(shadow);
                if (imbalanceOn) full.soft = static_cast<double>(raw.cut - raw.imbalance);
                const Score incremental = editor.score();
                if (!sameScore(full, incremental) && ++mismatches <= 3) {
                    std::printf("  MISMATCH seed %llu step %d: incremental=%s full=%s\n",
                                static_cast<unsigned long long>(seed), step,
                                incremental.str().c_str(), full.str().c_str());
                }
                const std::vector<std::vector<double>> costs = rawTotals(calculator);
                if (costs != expectedUnitCosts(raw) && ++costMismatches <= 3) {
                    std::printf("  RAW COST MISMATCH seed %llu step %d\n",
                                static_cast<unsigned long long>(seed), step);
                }
                for (std::size_t c = 0; c < costs.size() && c < 6; ++c) {
                    bool any = false;
                    for (const double cost : costs[c]) any = any || cost != 0.0;
                    nonZero[c] += any ? 1 : 0;
                }
            }
        }
        const std::string assembly =
                imbalanceOn ? " (partitionImbalanceCost at weight 1)" : " (defaults)";
        check(mismatches == 0,
              std::to_string(moves) + " random moves, incremental == full recompute" + assembly);
        check(costMismatches == 0,
              std::to_string(moves) +
                      " random moves, every unit cost == ScoreCalculator::rawCosts" + assembly);
        for (int c = 0; c < 6; ++c) {
            check(nonZero[c] > 0, std::string(kCatalogNames[c]) + " non-zero on " +
                                          std::to_string(nonZero[c]) + "/" + std::to_string(moves) +
                                          " moves" + assembly);
        }
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Dropping one component removes exactly its term.
// -------------------------------------------------------------------------
void verifyPartialAssemblies() {
    std::printf("== (c) Partial assemblies ==\n");
    MaxCutProblem p = fixtureH();
    p.side = {0, 1, 1, 1};  // every hard term positive, cut 2, imbalance 2
    ScoreCalculator::prepare(p);
    const RawCosts raw = ScoreCalculator::rawCosts(p);
    struct Case {
        const char* name;
        Score expected;
    };
    // The default score is -5hard/2soft; each line is the score without one.
    const Case cases[] = {
            {"fixedPartitionCost", {-5 + raw.fixedTotal(), 2.0}},
            {"samePartitionPairsCost", {-5 + raw.sameTotal(), 2.0}},
            {"differentPartitionPairsCost", {-5 + raw.differentTotal(), 2.0}},
            {"partitionSizeBoundsCost", {-5 + raw.sizeTotal(), 2.0}},
            {"partitionImbalanceCost", {-5, 2.0}},  // weight 0: nothing to remove
            {"cutWeightCost", {-5, 0.0}},
    };
    for (const Case& c : cases) {
        DecomposedCalculator calculator;
        defaultConstraints().drop(c.name).into(calculator);
        const Score score = scoreWith(calculator, p);
        check(sameScore(score, c.expected),
              std::string("without ") + c.name + " -> " + score.str());
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Overrides.
// -------------------------------------------------------------------------
void verifyOverrides() {
    std::printf("== (d) Weight / level / exponent overrides ==\n");
    MaxCutProblem p = fixtureH();
    p.side = {0, 1, 1, 1};
    {
        DecomposedCalculator calculator;
        defaultConstraints().weight("partitionImbalanceCost", 1).into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-5, 0.0}),
              "partitionImbalanceCost at weight 1: soft 2 - 2 = 0");
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints().weight("cutWeightCost", 3).into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-5, 6.0}),
              "cutWeightCost at weight 3: soft 6");
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints().soft("partitionSizeBoundsCost").into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-3, 0.0}),
              "partitionSizeBoundsCost made soft: hard -3, soft 2 - 2 = 0");
    }
    {
        DecomposedCalculator calculator;
        defaultConstraints()
                .hard("partitionImbalanceCost")
                .weight("partitionImbalanceCost", 1)
                .into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-7, 2.0}),
              "partitionImbalanceCost made hard: -5 - 2");
    }
    {
        // Exponent 2 shapes each side: the size bounds [1, 1] stay 1 + 1; a
        // side holding two violations becomes 2^2 = 4.
        DecomposedCalculator calculator;
        defaultConstraints().exponent("partitionSizeBoundsCost", 2).into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-5, 2.0}),
              "size bounds [1, 1] at exponent 2: still 2");
        MaxCutProblem q = fixtureH();
        q.minPartitionSize.reset();
        q.maxPartitionSize.reset();
        q.differentPartitionPairs.clear();
        q.fixedSide = {1, 1, 1, kUnassigned};
        q.side = {0, 0, 0, 1};  // A, B, C off their side 1, all on side 0 (A, B together)
        DecomposedCalculator plain;
        addDefaultConstraints(plain);
        DecomposedCalculator shaped;
        defaultConstraints().exponent("fixedPartitionCost", 2).into(shaped);
        check(scoreWith(plain, q).hard == -3 && scoreWith(shaped, q).hard == -9,
              "fixedPartitionCost [3, 0]: 3 at exponent 1, 9 at exponent 2 (per side)");
    }
    {
        // Exponent on the reward is sign-preserving: -(2^2) = -4, soft +4.
        DecomposedCalculator calculator;
        defaultConstraints().exponent("cutWeightCost", 2).into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-5, 4.0}),
              "cutWeightCost [-2] at exponent 2: soft 4");
    }
    {
        // Hard is rounded per side: 0.4 on [1, 1] gives round(0.4) * 2 = 0.
        DecomposedCalculator calculator;
        defaultConstraints().weight("partitionSizeBoundsCost", 0.4).into(calculator);
        check(scoreWith(calculator, p).hard == -3,
              "hard weight 0.4 on size [1, 1]: rounded per side to 0 + 0");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraint_totals() explains the score.
// -------------------------------------------------------------------------
void verifyConstraintTotals() {
    std::printf("== (e) constraintTotals() ==\n");
    MaxCutProblem p = randomInstance(30, 41);
    DecomposedCalculator calculator;
    defaultConstraints().weight("partitionImbalanceCost", 2).into(calculator);
    const Score score = scoreWith(calculator, p);
    double hard = 0.0;
    double soft = 0.0;
    std::vector<std::string> names;
    for (const optimaru::ConstraintTotal& total : calculator.constraint_totals()) {
        names.push_back(total.name);
        (total.is_hard ? hard : soft) += total.cost;
    }
    check(names == std::vector<std::string>({"fixedPartitionCost", "samePartitionPairsCost",
                                             "differentPartitionPairsCost",
                                             "partitionSizeBoundsCost", "partitionImbalanceCost",
                                             "cutWeightCost"}),
          "totals in catalog order");
    check(-hard == static_cast<double>(score.hard) && -soft == score.soft,
          "sum of the totals == the score (" + score.str() + ")");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) ScoreChecks::Full solve on the plugin path.
// -------------------------------------------------------------------------
bool fullAssertSolve(MaxCutProblem& p, ConstraintAssembly assembly, const std::string& label) {
    Solver::Config config;
    config.time_limit = std::chrono::seconds(10);
    config.step_limit = 4000;
    config.seed = 3;
    config.score_checks = optimaru::ScoreChecks::Full;
    config.ruin_recreate_unimproved_step_trigger = 200;
    config.ruin_recreate_polish_steps = 100;
    config.ruin_recreate_as_move_weight = 0.05;
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    try {
        Solver(config).solve(p, calculator);
    } catch (const optimaru::ScoreMismatchError& e) {
        std::printf("  ScoreCorruptionError: %s\n", e.what());
        check(false, label);
        return false;
    }
    check(true, label);
    return true;
}

void verifyFullAssertSolve() {
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    {
        MaxCutProblem p = randomInstance(30, 11, 0.0, /*bounds=*/false);
        if (fullAssertSolve(p, defaultConstraints(),
                            "FULL_ASSERT plugin solve (flips, swaps, ALNS kicks)")) {
            MaxCutProblem rescored = p;
            check(sameScore(p.score, ScoreCalculator::score(rescored)),
                  "plugin-reported score == monolithic full recompute");
        }
    }
    {
        MaxCutProblem p = randomInstance(30, 12, 0.0, /*bounds=*/false);
        fullAssertSolve(p,
                        defaultConstraints()
                                .weight("partitionImbalanceCost", 1)
                                .exponent("cutWeightCost", 1.5),
                        "FULL_ASSERT plugin solve with the imbalance on and a shaped cut");
    }
    {
        MaxCutProblem p = randomInstance(30, 13, 0.0, /*bounds=*/false);
        p.minPartitionSize = p.maxPartitionSize = 15;
        int startSideOne = 0;  // the start partition: fixed side, else index % 2
        for (int v = 0; v < p.vertexCount(); ++v)
            startSideOne += (p.fixedSide[v] >= 0 ? p.fixedSide[v] : v % 2) == 1 ? 1 : 0;
        fullAssertSolve(p, defaultConstraints(),
                        "FULL_ASSERT plugin solve in exact-size mode (swaps only)");
        check(p.sideSize(1) == startSideOne && p.sideSize(0) == p.vertexCount() - startSideOne,
              "the exact-size solve kept the start partition's side sizes");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) The assembly builder.
// -------------------------------------------------------------------------
void verifyAssemblyBuilder() {
    std::printf("== (g) defaultConstraints() builder ==\n");
    MaxCutProblem p = randomInstance(25, 17);
    DecomposedCalculator byHand;
    {
        byHand.emplace<SamePartitionPairsCost>();
        byHand.emplace<DifferentPartitionPairsCost>(4.0);
        auto size = std::make_unique<PartitionSizeBoundsCost>();
        size->set_level(optimaru::Level::Soft);
        byHand.add(std::move(size));
        byHand.emplace<PartitionImbalanceCost>(1.0);
        byHand.emplace<CutWeightCost>();
    }
    const Score expected = scoreWith(byHand, p);
    DecomposedCalculator built;
    defaultConstraints()
            .weight("differentPartitionPairsCost", 4)
            .soft("partitionSizeBoundsCost")
            .weight("partitionImbalanceCost", 1)
            .drop("fixedPartitionCost")
            .into(built);
    check(sameScore(scoreWith(built, p), expected),
          "named edits == the same assembly listed by hand");
    DecomposedCalculator plain;
    defaultConstraints().into(plain);
    check(sameScore(scoreWith(plain, p), ScoreCalculator::score(p)),
          "defaultConstraints() alone == the monolithic score");
    bool threw = false;
    try {
        defaultConstraints().weight("cutWeightCosts", 4);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "a misspelled constraint name throws instead of doing nothing");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) Raw costs by hand, read through constraint_costs().
// -------------------------------------------------------------------------
using Costs = std::vector<double>;

Costs rawCosts(MaxCutProblem& problem, const std::string& name) {
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    scoreWith(calculator, problem);
    for (const auto& entry : calculator.constraint_costs()) {
        if (entry.name == name) return entry.unit_costs;
    }
    return {};
}

void verifyRawCostsByHand() {
    std::printf("== (h) Raw costs by hand, positive and zero ==\n");
    {
        MaxCutProblem p = fixtureH();
        p.side = {0, 1, 1, 1};
        check(rawCosts(p, "fixedPartitionCost") == Costs{1.0, 0.0},
              "fixedPartitionCost: [1, 0] (A off side 1)");
        check(rawCosts(p, "samePartitionPairsCost") == Costs{1.0, 0.0},
              "samePartitionPairsCost: [1, 0] ((A, B) split, A on side 0)");
        check(rawCosts(p, "differentPartitionPairsCost") == Costs{0.0, 1.0},
              "differentPartitionPairsCost: [0, 1] (B, C on side 1)");
        check(rawCosts(p, "partitionSizeBoundsCost") == Costs{1.0, 1.0},
              "partitionSizeBoundsCost: [1, 1] (1 and 3 for [2, 2])");
        check(rawCosts(p, "partitionImbalanceCost") == Costs{2.0}, "partitionImbalanceCost: [2]");
        check(rawCosts(p, "cutWeightCost") == Costs{-2.0},
              "cutWeightCost: [-2] (the reward, A-B cut)");
        p.side = {1, 1, 0, 0};
        for (const char* name : {"fixedPartitionCost", "samePartitionPairsCost",
                                 "differentPartitionPairsCost", "partitionSizeBoundsCost"}) {
            check(rawCosts(p, name) == Costs({0.0, 0.0}),
                  std::string(name) + ": [0, 0] on (A, B | C, D)");
        }
        check(rawCosts(p, "partitionImbalanceCost") == Costs{0.0},
              "partitionImbalanceCost: [0] on 2 + 2");
        check(rawCosts(p, "cutWeightCost") == Costs{-3.0}, "cutWeightCost: [-3] (B-C cut)");
        p.side = {1, 1, 1, 1};
        const Costs cut = rawCosts(p, "cutWeightCost");
        check(cut == Costs{0.0} && !std::signbit(cut[0]),
              "cutWeightCost: [0.0] (not -0.0) with nothing cut");
    }
    {
        // A self-loop is never cut; parallel edges add up.
        MaxCutProblem p = graph({"A", "B"}, {{0, 0, 5}, {0, 1, 1}, {1, 0, 2}});
        p.side = {0, 1};
        check(rawCosts(p, "cutWeightCost") == Costs{-3.0},
              "cutWeightCost: [-3] (self-loop 5 never cut)");
    }
    {
        // Self-pairs.
        MaxCutProblem p = graph({"A", "B"}, {});
        p.samePartitionPairs = {{0, 0}};
        p.differentPartitionPairs = {{1, 1}};
        p.side = {0, 1};
        check(rawCosts(p, "samePartitionPairsCost") == Costs({0.0, 0.0}),
              "a same self-pair: [0, 0]");
        check(rawCosts(p, "differentPartitionPairsCost") == Costs({0.0, 1.0}),
              "a different self-pair: [0, 1]");
    }
    {
        // The -1 rules, the same as the monolith's (mc/domain.hpp).
        MaxCutProblem p = graph({"A", "B", "C"}, {{0, 1, 4}, {1, 2, 5}});
        p.fixedSide[2] = 1;
        p.samePartitionPairs = {{0, 1}, {1, 0}};
        p.differentPartitionPairs = {{0, 2}};
        p.side = {0, kUnassigned, kUnassigned};
        check(rawCosts(p, "samePartitionPairsCost") == Costs({1.0, 0.0}),
              "-1: (A, B) split while B is unassigned; (B, A) charges nothing");
        check(rawCosts(p, "differentPartitionPairsCost") == Costs({0.0, 0.0}), "-1: never joined");
        check(rawCosts(p, "fixedPartitionCost") == Costs({0.0, 0.0}),
              "-1: an unassigned fixed vertex costs 0");
        check(rawCosts(p, "cutWeightCost") == Costs{0.0}, "-1: no edge cut");
        check(rawCosts(p, "partitionImbalanceCost") == Costs{1.0},
              "-1: the imbalance counts assigned vertices");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) The catalog and the reward guards.
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
           double weight, bool is_global, bool reward = false) {
    return row.name == name && row.default_level == Component::kDefaultLevel &&
           row.default_weight == Component::kDefaultWeight && row.default_level == level &&
           row.default_weight == weight && row.level_flippable == !reward && row.reward == reward &&
           row.is_global == is_global && row.default_enabled == (weight > 0.0);
}

void verifyCatalog() {
    std::printf("== (i) Catalog and guards ==\n");
    using optimaru::Level;
    const auto rows = defaultConstraints().info();
    check(rows.size() == 6 &&
                  rowIs<FixedPartitionCost>(rows[0], "fixedPartitionCost", Level::Hard, 1.0,
                                            false) &&
                  rowIs<SamePartitionPairsCost>(rows[1], "samePartitionPairsCost", Level::Hard, 1.0,
                                                false) &&
                  rowIs<DifferentPartitionPairsCost>(rows[2], "differentPartitionPairsCost",
                                                     Level::Hard, 1.0, false) &&
                  rowIs<PartitionSizeBoundsCost>(rows[3], "partitionSizeBoundsCost", Level::Hard,
                                                 1.0, false) &&
                  rowIs<PartitionImbalanceCost>(rows[4], "partitionImbalanceCost", Level::Soft, 0.0,
                                                true) &&
                  rowIs<CutWeightCost>(rows[5], "cutWeightCost", Level::Soft, 1.0, true,
                                       /*reward=*/true),
          "info() == the components' constants, in catalog order");
    check(!rows[4].default_enabled,
          "partitionImbalanceCost is opt-in: default_enabled false at weight 0");
    check(defaultConstraints().names() ==
                  std::vector<std::string>({"fixedPartitionCost", "samePartitionPairsCost",
                                            "differentPartitionPairsCost",
                                            "partitionSizeBoundsCost", "partitionImbalanceCost",
                                            "cutWeightCost"}),
          "names() in catalog order");
    check(throwsInvalid([] { defaultConstraints().hard("cutWeightCost"); }),
          "hard() on the cutWeightCost reward throws");
    check(throwsInvalid([] { CutWeightCost().set_level(Level::Hard); }),
          "CutWeightCost::setLevel(Hard) throws");
    check(throwsInvalid([] { CutWeightCost().set_level(Level::Medium); }),
          "the reward's level is not flippable: setLevel(Medium) throws");
    check(!throwsInvalid([] { defaultConstraints().soft("cutWeightCost"); }),
          "soft() on the reward re-states its level: a no-op");
    check(throwsInvalid([] { defaultConstraints().weight("fixedPartitionCost", -1); }),
          "a negative weight throws");
    check(throwsInvalid([] { defaultConstraints().exponent("fixedPartitionCost", 0); }),
          "a zero exponent throws");
    {
        // A medium level needs a domain medium fold: max-cut declares none.
        MaxCutProblem p = fixtureH();
        p.side = {1, 1, 0, 0};
        ScoreCalculator::prepare(p);
        DecomposedCalculator calculator;
        defaultConstraints().level("partitionSizeBoundsCost", Level::Medium).into(calculator);
        bool threw = false;
        try {
            calculator.rebuild(p);
        } catch (const std::logic_error&) {
            threw = true;
        }
        check(threw, "a medium constraint is refused at reset (no medium fold)");
    }
    {
        // The components refuse an unprepared problem.
        MaxCutProblem p = fixtureH();
        p.side = {1, 1, 0, 0};
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        bool threw = false;
        try {
            calculator.rebuild(p);
        } catch (const std::logic_error&) {
            threw = true;
        }
        check(threw, "an unprepared problem is refused");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (j) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with add_instances(), as the Python bindings
// build it from a pick list. The four unit constraints shape each side, the
// two globals their total.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string& name) {
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

// Fixture H positive: fixed [1, 0], same [1, 0], different [0, 1], size
// [1, 1], imbalance 2, cut -2.
MaxCutProblem hPositive() {
    MaxCutProblem p = fixtureH();
    p.side = {0, 1, 1, 1};
    return p;
}

Score scoreOf(ConstraintAssembly assembly, MaxCutProblem p) {
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    return scoreWith(calculator, p);
}

// Every kind repeated or shifted at least once: per-member deltas, pair
// deltas, the O(1) re-cost, both member-change globals, the reward in both
// modes, a fractional hard weight on a threshold offset (hard refuses the
// literal formula with an offset, so the literal copies of the hard unit
// constraints sit on soft); and repeats on the other level than their
// default: the size bounds demoted to soft, the imbalance promoted to hard
// (threshold).
ConstraintAssembly repeatedAssembly() {
    ConstraintAssembly assembly = defaultConstraints();
    assembly.weight("partitionImbalanceCost", 2.0).offset("partitionImbalanceCost", 3.0);
    assembly.offset("cutWeightCost", 5.0).clip("cutWeightCost", false);
    assembly.add_instances(std::move(copyOf("fixedPartitionCost")
                                             .weight("fixedPartitionCost", 1.5)
                                             .offset("fixedPartitionCost", 0.5)));
    assembly.add_instances(std::move(copyOf("fixedPartitionCost")
                                             .soft("fixedPartitionCost")
                                             .weight("fixedPartitionCost", 1.5)
                                             .offset("fixedPartitionCost", 0.5)
                                             .clip("fixedPartitionCost", false)));
    assembly.add_instances(std::move(copyOf("samePartitionPairsCost")
                                             .weight("samePartitionPairsCost", 2.0)
                                             .exponent("samePartitionPairsCost", 2.0)
                                             .offset("samePartitionPairsCost", 1.0)));
    assembly.add_instances(std::move(copyOf("differentPartitionPairsCost")
                                             .soft("differentPartitionPairsCost")
                                             .offset("differentPartitionPairsCost", -1.0)
                                             .clip("differentPartitionPairsCost", false)));
    assembly.add_instances(std::move(copyOf("partitionSizeBoundsCost")
                                             .soft("partitionSizeBoundsCost")
                                             .offset("partitionSizeBoundsCost", 2.0)));
    assembly.add_instances(std::move(copyOf("partitionImbalanceCost")
                                             .weight("partitionImbalanceCost", 0.5)
                                             .exponent("partitionImbalanceCost", 2.0)
                                             .offset("partitionImbalanceCost", -1.0)
                                             .clip("partitionImbalanceCost", false)));
    assembly.add_instances(std::move(copyOf("cutWeightCost")
                                             .weight("cutWeightCost", 0.25)
                                             .exponent("cutWeightCost", 1.5)
                                             .offset("cutWeightCost", 10.0)));
    assembly.add_instances(std::move(copyOf("partitionImbalanceCost")
                                             .hard("partitionImbalanceCost")
                                             .weight("partitionImbalanceCost", 2.0)
                                             .offset("partitionImbalanceCost", 1.0)));
    return assembly;
}

void verifyRepeatsAndOffsets() {
    std::printf("== (j) Repeated components, offset / clip ==\n");
    {
        // size per side [1, 1]: (1 + 1) + 2 x (1^2 + 1^2) = 6 hard.
        ConstraintAssembly assembly = copyOf("partitionSizeBoundsCost");
        assembly.add_instances(std::move(copyOf("partitionSizeBoundsCost")
                                                 .weight("partitionSizeBoundsCost", 2.0)
                                                 .exponent("partitionSizeBoundsCost", 2.0)));
        MaxCutProblem p = hPositive();
        DecomposedCalculator calculator;
        assembly.into(calculator);
        const Score score = scoreWith(calculator, p);
        check(score.hard == -6 && score.soft == 0.0,
              "size bounds + 2 x size^2 per side: -(2 + 4) hard");
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 2 && entries[0].unit_costs == entries[1].unit_costs &&
                      entries[0].unit_costs == Costs{1.0, 1.0} && entries[1].weight == 2.0 &&
                      entries[1].exponent == 2.0 && entries[1].default_weight == 1.0 &&
                      entries[0].name == entries[1].name,
              "two instances, the same raw costs per side, each with its own knobs");
    }
    {
        // Each instance on its own level: the soft imbalance (2, at weight 1)
        // next to a hard repeat, 2 x (2 - 1) beyond its offset.
        ConstraintAssembly assembly = copyOf("partitionImbalanceCost");
        assembly.weight("partitionImbalanceCost", 1.0);
        assembly.add_instances(std::move(copyOf("partitionImbalanceCost")
                                                 .hard("partitionImbalanceCost")
                                                 .weight("partitionImbalanceCost", 2.0)
                                                 .offset("partitionImbalanceCost", 1.0)));
        MaxCutProblem p = hPositive();
        DecomposedCalculator calculator;
        assembly.into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-2, -2.0}),
              "soft imbalance + a hard repeat beyond 1: -2 hard / -2 soft");
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 2 && entries[0].level == optimaru::Level::Soft &&
                      entries[1].level == optimaru::Level::Hard &&
                      entries[1].default_level == optimaru::Level::Soft,
              "the repeat reports its own level, the default stays soft");
    }
    {
        ConstraintAssembly assembly = copyOf("cutWeightCost");
        assembly.add_instances(copyOf("cutWeightCost"));
        check(scoreOf(std::move(assembly), hPositive()).soft == 4.0,
              "the cut reward picked twice: +2 x 2");
    }
    {
        const MaxCutProblem p = hPositive();
        // The four unit constraints are hard by default, which refuses the
        // literal formula with an offset, so its numbers are checked on soft
        // (not rounded); hard rounds a threshold per side.
        check(sameScore(scoreOf(copyOf("differentPartitionPairsCost")
                                        .soft("differentPartitionPairsCost")
                                        .offset("differentPartitionPairsCost", 1.0)
                                        .clip("differentPartitionPairsCost", false),
                                p),
                        Score{0, 1.0}),
              "soft literal offset 1 on different [0, 1]: (0 - 1) + (1 - 1), the side of raw 0 is "
              "rewarded");
        check(scoreOf(copyOf("differentPartitionPairsCost")
                              .offset("differentPartitionPairsCost", 1.0),
                      p)
                              .hard == 0,
              "threshold offset 1 on different [0, 1]: 0");
        check(sameScore(scoreOf(copyOf("samePartitionPairsCost")
                                        .soft("samePartitionPairsCost")
                                        .offset("samePartitionPairsCost", 2.0)
                                        .clip("samePartitionPairsCost", false),
                                p),
                        Score{0, 3.0}),
              "soft literal offset 2 on same [1, 0]: (1 - 2) + (0 - 2) = -3, +3");
        check(scoreOf(copyOf("samePartitionPairsCost").offset("samePartitionPairsCost", -1.0), p)
                              .hard == -2,
              "a negative threshold offset adds to the non-zero side only: 1 + 1");
        check(sameScore(scoreOf(copyOf("samePartitionPairsCost")
                                        .soft("samePartitionPairsCost")
                                        .offset("samePartitionPairsCost", -1.0)
                                        .clip("samePartitionPairsCost", false),
                                p),
                        Score{0, -3.0}),
              "a negative soft literal offset adds to both sides: (1 + 1) + (0 + 1)");
        check(sameScore(scoreOf(copyOf("samePartitionPairsCost")
                                        .soft("samePartitionPairsCost")
                                        .offset("samePartitionPairsCost", 1.5)
                                        .clip("samePartitionPairsCost", false),
                                p),
                        Score{0, 2.0}),
              "a soft literal offset is not rounded: (1 - 1.5) + (0 - 1.5) = -2, +2");
        check(sameScore(scoreOf(copyOf("partitionSizeBoundsCost")
                                        .offset("partitionSizeBoundsCost", 0.5),
                                p),
                        Score{-2, 0.0}),
              "a hard threshold offset rounds per side: llround(-0.5) + llround(-0.5) = -2");
        check(sameScore(scoreOf(copyOf("partitionSizeBoundsCost")
                                        .soft("partitionSizeBoundsCost")
                                        .offset("partitionSizeBoundsCost", 1.0)
                                        .exponent("partitionSizeBoundsCost", 3.0)
                                        .clip("partitionSizeBoundsCost", false),
                                p),
                        Score{0, 0.0}),
              "soft literal offset 1, exponent 3 on size [1, 1]: 0^3 + 0^3");
        check(scoreOf(copyOf("partitionImbalanceCost")
                              .weight("partitionImbalanceCost", 1.0)
                              .offset("partitionImbalanceCost", 1.0),
                      p)
                              .soft == -1.0,
              "the imbalance total 2 with a threshold offset 1: -1 soft");
        check(scoreOf(copyOf("partitionImbalanceCost")
                              .weight("partitionImbalanceCost", 1.0)
                              .offset("partitionImbalanceCost", 3.0)
                              .clip("partitionImbalanceCost", false),
                      p)
                              .soft == 1.0,
              "the imbalance total 2 with a literal offset 3: shaped once, 2 - 3 = -1, +1 soft");
        check(scoreOf(copyOf("partitionImbalanceCost")
                              .offset("partitionImbalanceCost", 3.0)
                              .clip("partitionImbalanceCost", false),
                      p)
                              .soft == 0.0,
              "the default weight 0 contributes exactly 0, offset or not");
        check(scoreOf(copyOf("cutWeightCost").offset("cutWeightCost", 1.0), p).soft == 1.0,
              "the cut reward 2 with a threshold offset 1: only the 1 beyond it");
        check(scoreOf(copyOf("cutWeightCost")
                              .offset("cutWeightCost", 1.0)
                              .clip("cutWeightCost", false),
                      p)
                              .soft == 3.0,
              "the cut reward 2 with a literal offset 1: -2 - 1 = -3, +3 soft");
        DecomposedCalculator calculator;
        copyOf("fixedPartitionCost")
                .soft("fixedPartitionCost")
                .offset("fixedPartitionCost", 2.5)
                .clip("fixedPartitionCost", false)
                .into(calculator);
        MaxCutProblem q = hPositive();
        scoreWith(calculator, q);
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 1 && entries[0].offset == 2.5 && !entries[0].clip &&
                      !entries[0].scoped && entries[0].unit_costs == Costs{1.0, 0.0},
              "constraintCosts() reports the offset and clip, the raw side costs unshaped");
    }
    {
        // On hard the literal formula with an offset is refused at reset,
        // whichever sign and setter order, for a hard default unit constraint
        // and for the soft imbalance moved to hard: +1 on different [0, 1]
        // would reward the clean side and score a violated pair +1 hard, -1
        // would charge both sides of a clean partition.
        auto refusal = [](const std::string& name) {
            return "Constraint '" + name +
                   "' cannot use clip=false with a non-zero offset on a level that counts "
                   "toward the hard score (hard 0 must mean no violation)";
        };
        auto resetRefusal = [](ConstraintAssembly assembly) {
            try {
                scoreOf(std::move(assembly), hPositive());
            } catch (const std::logic_error& error) {
                return std::string(error.what());
            }
            return std::string();
        };
        check(resetRefusal(copyOf("differentPartitionPairsCost")
                                   .offset("differentPartitionPairsCost", 1.0)
                                   .clip("differentPartitionPairsCost", false)) ==
                              refusal("differentPartitionPairsCost") &&
                      resetRefusal(copyOf("differentPartitionPairsCost")
                                           .clip("differentPartitionPairsCost", false)
                                           .offset("differentPartitionPairsCost", -1.0)) ==
                              refusal("differentPartitionPairsCost"),
              "different-side literal offset +1 / -1 on its default hard: refused at reset");
        check(resetRefusal(copyOf("partitionImbalanceCost")
                                   .weight("partitionImbalanceCost", 1.0)
                                   .offset("partitionImbalanceCost", 3.0)
                                   .clip("partitionImbalanceCost", false)
                                   .hard("partitionImbalanceCost")) ==
                      refusal("partitionImbalanceCost"),
              "imbalance literal offset moved to hard afterwards: refused at reset");
        ConstraintAssembly repeated = copyOf("fixedPartitionCost");
        repeated.add_instances(std::move(copyOf("fixedPartitionCost")
                                                 .offset("fixedPartitionCost", 2.0)
                                                 .clip("fixedPartitionCost", false)));
        check(resetRefusal(std::move(repeated)) == refusal("fixedPartitionCost"),
              "a hard literal repeat next to the default: refused at reset");
        check(resetRefusal(copyOf("differentPartitionPairsCost")
                                   .offset("differentPartitionPairsCost", 1.0))
                              .empty() &&
                      resetRefusal(copyOf("differentPartitionPairsCost")
                                           .clip("differentPartitionPairsCost", false))
                              .empty() &&
                      resetRefusal(copyOf("differentPartitionPairsCost")
                                           .offset("differentPartitionPairsCost", 1.0)
                                           .clip("differentPartitionPairsCost", false)
                                           .soft("differentPartitionPairsCost"))
                              .empty() &&
                      resetRefusal(copyOf("cutWeightCost")
                                           .offset("cutWeightCost", 1.0)
                                           .clip("cutWeightCost", false))
                              .empty(),
              "a threshold offset or clip=false with offset 0 on hard, the literal offset on soft "
              "and on the soft reward are allowed");
    }
    {
        // Random walk: incremental == a fresh calculator of the same assembly.
        int mismatches = 0;
        int moves = 0;
        double drift = 0.0;
        for (std::uint64_t seed = 7; seed < 10; ++seed) {
            MaxCutProblem p = randomInstance(40, seed, 0.1);
            ScoreCalculator::prepare(p);
            DecomposedCalculator calculator;
            repeatedAssembly().into(calculator);
            optimaru::Editor<MaxCutProblem> editor(p, calculator);
            std::mt19937_64 rng(seed * 321);
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            const int n = p.vertexCount();
            for (int step = 0; step < 1000; ++step, ++moves) {
                const double u = unit(rng);
                if (u < 0.6) {
                    const int v = static_cast<int>(rng() % n);
                    editor.assign(v, unit(rng) < 0.15 ? kUnassigned : static_cast<int>(rng() % 2));
                } else {
                    editor.begin_batch();
                    const int a = static_cast<int>(rng() % n);
                    const int b = static_cast<int>(rng() % n);
                    const int va = p.side[a];
                    editor.assign(a, p.side[b]);
                    editor.assign(b, va);
                    if (u > 0.9)
                        editor.assign(static_cast<int>(rng() % n), static_cast<int>(rng() % 2));
                    editor.end_batch();
                }
                MaxCutProblem shadow = p;
                DecomposedCalculator fresh;
                repeatedAssembly().into(fresh);
                const Score full = scoreWith(fresh, shadow);
                const Score incremental = editor.score();
                drift = std::max(drift, std::fabs(full.soft - incremental.soft));
                if (!optimaru::scores_agree(incremental, full) && ++mismatches <= 3) {
                    std::printf("  MISMATCH seed %llu step %d: incremental=%s full=%s\n",
                                static_cast<unsigned long long>(seed), step,
                                incremental.str().c_str(), full.str().c_str());
                }
            }
        }
        std::printf("    largest soft drift %.3g\n", drift);
        check(mismatches == 0, std::to_string(moves) +
                                       " random moves with repeated, shifted components on both "
                                       "levels: incremental == fresh");
    }
    {
        MaxCutProblem p = randomInstance(30, 11, 0.0, /*bounds=*/false);
        if (fullAssertSolve(p, repeatedAssembly(),
                            "FULL_ASSERT solve with repeated, shifted components")) {
            DecomposedCalculator fresh;
            repeatedAssembly().into(fresh);
            check(optimaru::scores_agree(p.score, scoreWith(fresh, p)),
                  "the reported score == a fresh recompute of the same assembly");
        }
    }
    std::printf("\n");
}

}  // namespace

int main() {
    verifyAssemblyParity();
    verifyIncremental();
    verifyPartialAssemblies();
    verifyOverrides();
    verifyConstraintTotals();
    verifyFullAssertSolve();
    verifyAssemblyBuilder();
    verifyRawCostsByHand();
    verifyCatalog();
    verifyRepeatsAndOffsets();
    if (failures) {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}

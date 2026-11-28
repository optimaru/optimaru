// Plugin-assembly (bs/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure:
//   (a) the default assembly scores like the monolithic ScoreCalculator on
//       random assignments (identical with whole costs, within rounding with
//       fractional ones)
//   (b) incremental == a fresh calculator across a random walk through the
//       engine's Editor, with every constraint picked (boxTypeCost included)
//   (c) each component's raw cost by hand: a violating and a clean case
//   (d) the catalog read off the default assembly, and the knob guards
//   (e) partial assemblies, re-weights, level flips, a repeat with an offset
//   (f) boxTypeCost: a weight on it is the fixed cost of one more size, and
//       the solve with it stocks fewer sizes
//   (g) a solve under ScoreChecks::Full through the assembly, and its score
//       against the monolithic recompute
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bs/constraints.hpp"
#include "bs/model/domain.hpp"
#include "bs/score/score_calculator.hpp"
#include "bs/solver.hpp"
#include "bs_fixtures.hpp"
#include "optimaru/solution/editor.hpp"
#include "optimaru/testing/check.hpp"

using namespace bs;
using namespace bs_fixtures;

namespace {

using optimaru::testing::check;
using optimaru::testing::failures;

Score scoreWith(DecomposedCalculator& calculator, BoxSizingProblem& problem) {
    ScoreCalculator::prepare(problem);
    calculator.rebuild(problem);
    return calculator.score();
}

Score scoreWith(ConstraintAssembly assembly, BoxSizingProblem& problem) {
    DecomposedCalculator calculator;
    assembly.into(calculator);
    return scoreWith(calculator, problem);
}

void randomAssignment(BoxSizingProblem& problem, std::mt19937_64& rng) {
    for (Parcel& parcel : problem.parcels) {
        parcel.box = static_cast<int>(rng() % (problem.boxes.size() + 1)) - 1;  // -1 too
    }
}

// -------------------------------------------------------------------------
// (a) Default assembly == monolithic score.
// -------------------------------------------------------------------------
void verifyAssemblyParity() {
    std::printf("== (a) Default assembly == monolithic score ==\n");
    for (double costStep : {0.0, 250.0, 0.37}) {
        BoxSizingProblem p = randomInstance(10, 50, 4, 9, costStep);
        p.boxes[3].required = true;
        p.boxes[5].maxWeight = 6000;
        std::mt19937_64 rng(4);
        int mismatches = 0;
        const int rounds = 300;
        for (int round = 0; round < rounds; ++round) {
            randomAssignment(p, rng);
            DecomposedCalculator calculator;
            addDefaultConstraints(calculator);
            const Score plugin = scoreWith(calculator, p);
            const Score monolith = ScoreCalculator::score(p);
            // Whole costs: every sum is exact, so the doubles are identical.
            const bool same = costStep == 0.37 ? sameScore(plugin, monolith) : plugin == monolith;
            if (!same) ++mismatches;
        }
        check(mismatches == 0, std::to_string(rounds) + " random assignments, cost step " +
                                       std::to_string(costStep) + ": the same score");
    }
    std::printf("\n");
}

ConstraintAssembly everyConstraint() {
    ConstraintAssembly assembly = defaultConstraints();
    assembly.weight("boxTypeCost", 1500.0);
    return assembly;
}

// -------------------------------------------------------------------------
// (b) Incremental == fresh recompute, every constraint picked.
// -------------------------------------------------------------------------
void verifyIncremental() {
    std::printf("== (b) Incremental == fresh recompute ==\n");
    BoxSizingProblem p = randomInstance(9, 40, 3, 13, 250.0);
    p.boxes[2].required = true;
    p.boxes[4].maxWeight = 5000;
    ScoreCalculator::prepare(p);
    DecomposedCalculator calculator;
    everyConstraint().into(calculator);
    optimaru::Editor<BoxSizingProblem> editor(p, calculator);
    std::mt19937_64 rng(99);
    int mismatches = 0;
    const int steps = 4000;
    for (int step = 0; step < steps; ++step) {
        const int parcel = static_cast<int>(rng() % p.parcels.size());
        const int box = static_cast<int>(rng() % (p.boxes.size() + 1)) - 1;
        if (step % 7 == 0) {  // a batch of two writes, as the moves on the set of sizes make
            editor.begin_batch();
            editor.assign(parcel, box);
            editor.assign(static_cast<int>(rng() % p.parcels.size()), box);
            editor.end_batch();
        } else {
            editor.assign(parcel, box);
        }
        BoxSizingProblem shadow = p;
        if (!(editor.score() == scoreWith(everyConstraint(), shadow))) ++mismatches;
    }
    check(mismatches == 0, std::to_string(steps) + " random moves: incremental == fresh");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Raw costs by hand, read through constraint_costs().
// -------------------------------------------------------------------------
std::vector<double> rawCosts(BoxSizingProblem& problem, const std::string& name) {
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    scoreWith(calculator, problem);
    for (const auto& entry : calculator.constraint_costs()) {
        if (entry.name == name) return entry.unit_costs;
    }
    return {};
}

// B0 300 x 200 x 100 (carries 1,000, cost 2.5), B1 400 x 300 x 200 (required).
BoxSizingProblem twoBoxes() {
    BoxSizingProblem p;
    addBox(p, 300, 200, 100, /*maxWeight*/ 1000, /*cost*/ 2.5);
    addBox(p, 400, 300, 200, std::nullopt, 0.0, /*required*/ true);
    return p;
}

void verifyRawCostsByHand() {
    std::printf("== (c) Raw costs by hand ==\n");
    {
        // (120, 210, 330) against (100, 200, 300): 20 + 10 + 30 = 60.
        BoxSizingProblem p = twoBoxes();
        addParcel(p, 330, 120, 210, 0);
        check(rawCosts(p, "parcelFitCost") == std::vector<double>{60.0, 0.0},
              "parcelFitCost: 20 + 10 + 30 over the sorted sides");
        p.parcels[0].box = 1;
        check(rawCosts(p, "parcelFitCost") == std::vector<double>{0.0, 0.0},
              "parcelFitCost: it fits the larger box");
    }
    {
        BoxSizingProblem p = twoBoxes();
        addParcel(p, 100, 100, 100, 0, /*quantity*/ 9, /*weight*/ 1300);
        addParcel(p, 100, 100, 100, 0, 1, 1000);
        check(rawCosts(p, "boxWeightCost") == std::vector<double>{300.0, 0.0},
              "boxWeightCost: 300 over, the quantity does not multiply");
        p.parcels[0].box = 1;
        check(rawCosts(p, "boxWeightCost") == std::vector<double>{0.0, 0.0},
              "boxWeightCost: a box without a limit");
    }
    {
        // B0 holds 6,000,000; the parcel 3,000,000: 4 x 3,000,000.
        BoxSizingProblem p = twoBoxes();
        addParcel(p, 300, 100, 100, 0, /*quantity*/ 4);
        check(rawCosts(p, "voidVolumeCost") == std::vector<double>{12000000.0, 0.0},
              "voidVolumeCost: 4 x 3,000,000");
        p.parcels[0].height = 200;
        p.prepared = false;  // a fact changed
        check(rawCosts(p, "voidVolumeCost") == std::vector<double>{0.0, 0.0},
              "voidVolumeCost: a parcel that fills its box");
    }
    {
        BoxSizingProblem p = twoBoxes();
        addParcel(p, 100, 100, 100, 0, /*quantity*/ 4);
        addParcel(p, 100, 100, 100, 0, /*quantity*/ 2);
        addParcel(p, 100, 100, 100, 1, /*quantity*/ 50);
        check(rawCosts(p, "boxCost") == std::vector<double>{15.0, 0.0},
              "boxCost: 6 shipments x 2.5, and a box without a cost");
    }
    {
        // One size in use and one required: 2 against a limit of 1.
        BoxSizingProblem p = twoBoxes();
        addParcel(p, 100, 100, 100, 0);
        p.maxBoxTypes = 1;
        check(rawCosts(p, "boxTypeLimitCost") == std::vector<double>{1.0} &&
                      rawCosts(p, "boxTypeCost") == std::vector<double>{2.0},
              "boxTypeLimitCost 1 over, boxTypeCost counts 2 (one is required)");
        p.parcels[0].box = 1;
        check(rawCosts(p, "boxTypeLimitCost") == std::vector<double>{0.0} &&
                      rawCosts(p, "boxTypeCost") == std::vector<double>{1.0},
              "everything in the required size: within the limit, 1 size");
        p.maxBoxTypes.reset();
        p.parcels[0].box = 0;
        check(rawCosts(p, "boxTypeLimitCost") == std::vector<double>{0.0},
              "boxTypeLimitCost: no limit, no cost");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) The catalog and the guards.
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
           double weight, bool global) {
    return row.name == name && row.default_level == Component::kDefaultLevel &&
           row.default_weight == Component::kDefaultWeight && row.default_level == level &&
           row.default_weight == weight && row.level_flippable && !row.reward &&
           row.is_global == global && row.default_enabled == (weight != 0.0);
}

void verifyCatalog() {
    std::printf("== (d) Catalog and guards ==\n");
    const auto rows = defaultConstraints().info();
    const auto hard = optimaru::Level::Hard;
    const auto soft = optimaru::Level::Soft;
    check(rows.size() == 6 && rowIs<ParcelFitCost>(rows[0], "parcelFitCost", hard, 1.0, false) &&
                  rowIs<BoxWeightCost>(rows[1], "boxWeightCost", hard, 1.0, false) &&
                  rowIs<VoidVolumeCost>(rows[2], "voidVolumeCost", soft, 1.0, false) &&
                  rowIs<BoxCost>(rows[3], "boxCost", soft, 1.0, false) &&
                  rowIs<BoxTypeLimitCost>(rows[4], "boxTypeLimitCost", hard, 1.0, true) &&
                  rowIs<BoxTypeCost>(rows[5], "boxTypeCost", soft, 0.0, true),
          "info(): fit, weight (hard 1), void, cost (soft 1), limit (hard 1, global), "
          "type (soft 0, global, opt-in); all flippable, no reward");
    check(!throwsInvalid([] { defaultConstraints().soft("parcelFitCost"); }) &&
                  !throwsInvalid([] { defaultConstraints().hard("voidVolumeCost"); }),
          "every level flips both ways");
    check(throwsInvalid([] { defaultConstraints().weight("boxCost", -2.0); }),
          "a negative weight is refused");
    check(throwsInvalid([] { defaultConstraints().weight("boxCosts", 2.0); }),
          "a misspelled constraint name throws instead of doing nothing");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) Partial assemblies and overrides.
// -------------------------------------------------------------------------
void verifyOverrides() {
    std::printf("== (e) Partial assemblies and overrides ==\n");
    // B0 carries 1,000 at cost 2.5, B1 is required. P0 (x4, 1,300 g) in B0:
    // weight 300 over, void 4 x 5,000,000, cost 4 x 2.5 = 10. Two sizes.
    BoxSizingProblem p = twoBoxes();
    addParcel(p, 100, 100, 100, 0, /*quantity*/ 4, /*weight*/ 1300);
    {
        const Score score = scoreWith(defaultConstraints(), p);
        check(score.hard == -300 && score.soft == -20000010.0, "the defaults: -300 / -20000010");
    }
    {
        const Score score = scoreWith(std::move(defaultConstraints().only({"boxCost"})), p);
        check(score.hard == 0 && score.soft == -10.0, "only boxCost");
    }
    {
        const Score score = scoreWith(
                std::move(defaultConstraints().weight("boxCost", 1000.0).drop("voidVolumeCost")),
                p);
        check(score.hard == -300 && score.soft == -10000.0, "boxCost x 1000, no void");
    }
    {
        const Score score = scoreWith(
                std::move(defaultConstraints().soft("boxWeightCost").weight("boxWeightCost", 2.0)),
                p);
        check(score.hard == 0 && score.soft == -20000610.0, "boxWeightCost soft at weight 2");
    }
    {
        // boxTypeCost at 50: 2 sizes -> 100; squared: 4 -> 200.
        const Score linear = scoreWith(
                std::move(defaultConstraints().only({"boxTypeCost"}).weight("boxTypeCost", 50.0)),
                p);
        const Score squared = scoreWith(std::move(defaultConstraints()
                                                          .only({"boxTypeCost"})
                                                          .weight("boxTypeCost", 50.0)
                                                          .exponent("boxTypeCost", 2.0)),
                                        p);
        check(linear.soft == -100.0 && squared.soft == -200.0,
              "boxTypeCost: 50 per size; its exponent shapes the count");
    }
    {
        // A repeat: the void again, squared beyond 1,000,000 per box, at 1e-9.
        ConstraintAssembly assembly = defaultConstraints();
        ConstraintAssembly repeat = defaultConstraints();
        repeat.only({"voidVolumeCost"});
        repeat.weight("voidVolumeCost", 1e-9)
                .exponent("voidVolumeCost", 2.0)
                .offset("voidVolumeCost", 1000000.0);
        assembly.add_instances(std::move(repeat));
        const Score score = scoreWith(std::move(assembly), p);
        const double expected = -(20000010.0 + 1e-9 * 19000000.0 * 19000000.0);
        check(score.hard == -300 && std::fabs(score.soft - expected) < 1e-6,
              "a second voidVolumeCost pick with an exponent and an offset adds up");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) boxTypeCost is the fixed cost of one more size.
// -------------------------------------------------------------------------
void verifyBoxTypeCost() {
    std::printf("== (f) boxTypeCost: the fixed cost of a size ==\n");
    const auto solveWith = [](double typeWeight) {
        BoxSizingProblem p = randomInstance(16, 120, 0, 31);
        Solver::Config config;
        config.time_limit = std::chrono::seconds(20);
        config.step_limit = 4000;
        config.seed = 2;
        DecomposedCalculator calculator;
        defaultConstraints().weight("boxTypeCost", typeWeight).into(calculator);
        Solver(config).solve(p, calculator);
        return p;
    };
    const BoxSizingProblem free = solveWith(0.0);
    const BoxSizingProblem dear = solveWith(5.0e8);
    std::printf("    sizes: %d at no fixed cost, %d at 5e8 per size\n", free.boxTypesUsed(),
                dear.boxTypesUsed());
    check(free.score.hard == 0 && dear.score.hard == 0, "both solves are feasible");
    check(dear.boxTypesUsed() < free.boxTypesUsed(), "a fixed cost per size stocks fewer sizes");
    check(dear.voidVolume() >= free.voidVolume(), "and ships at least as much air");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) FULL_ASSERT solve through the assembly.
// -------------------------------------------------------------------------
void verifyFullAssertSolve() {
    std::printf("== (g) FULL_ASSERT plugin solve ==\n");
    for (bool everything : {false, true}) {
        BoxSizingProblem p = randomInstance(10, 40, 3, 21, 250.0);
        Solver::Config config;
        config.time_limit = std::chrono::seconds(20);
        config.step_limit = 5000;
        config.seed = 3;
        config.score_checks = optimaru::ScoreChecks::Full;
        config.ruin_recreate_unimproved_step_trigger = 200;
        config.ruin_recreate_polish_steps = 100;

        DecomposedCalculator calculator;
        if (everything) {
            everyConstraint().into(calculator);
        } else {
            addDefaultConstraints(calculator);
        }
        bool corrupted = false;
        try {
            Solver(config).solve(p, calculator);
        } catch (const optimaru::ScoreMismatchError& error) {
            corrupted = true;
            std::printf("  ScoreMismatchError: %s\n", error.what());
        }
        const std::string what = everything ? "every constraint" : "the defaults";
        check(!corrupted, "5,000 FULL_ASSERT steps, " + what + ": no score mismatch");
        check(p.score.hard == 0, "the plugin solve is feasible");
        if (!everything) {
            const Score reported = p.score;
            check(reported == ScoreCalculator::score(p),
                  "the plugin-reported score == the monolithic recompute");
        }
    }
    std::printf("\n");
}

}  // namespace

int main() {
    verifyAssemblyParity();
    verifyIncremental();
    verifyRawCostsByHand();
    verifyCatalog();
    verifyOverrides();
    verifyBoxTypeCost();
    verifyFullAssertSolve();
    if (failures == 0) {
        std::printf("All box-sizing plugin checks passed.\n");
        return 0;
    }
    std::printf("%d box-sizing plugin check(s) FAILED.\n", failures);
    return 1;
}

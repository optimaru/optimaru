// Plugin-assembly (qap/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 500 random
//       permutations and on partial / doubly-occupied assignments
//   (b) incremental score == full recompute across 3000 random moves driven
//       through the engine's ScoreDirector protocol (delta units and the
//       member-change globals), and the raw costs stay equal to a fresh reset
//   (c) partial assemblies: removing one component removes exactly its term
//   (d) weight / level / exponent overrides (units shape per location,
//       globals shape their total)
//   (e) constraintTotals() explains exactly the reported score;
//       constraintCosts() layout (units: one entry per location in location
//       order; globals: one total)
//   (f) a FULL_ASSERT solve(problem, calculator), default and overridden
//   (g) the registry names and their Python spelling
//   (h) each component's raw cost, hand-calculated: a positive and a zero
//       case per constraint (raw 0 = no violation)
//   (i) the catalog: info() off the default assembly == the components'
//       kDefaultWeight / kDefaultLevel; the knob guards
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

#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/python/names.hpp"
#include "qap/constraints.hpp"
#include "qap/domain.hpp"
#include "qap/score_calculator.hpp"
#include "qap/solver.hpp"
#include "qap_fixtures.hpp"

using namespace qap;
using namespace qap_test;

namespace
{

using Costs = std::vector<DecomposedCalculator::ConstraintCosts>;

// Scores `problem`'s current assignment with a freshly reset assembly.
Score scoreWith(DecomposedCalculator &calculator, QuadraticAssignmentProblem &problem)
{
    ScoreCalculator::prepare(problem);
    calculator.resetWorkingSolution(problem);
    return calculator.calculateScore();
}

Score scoreWith(ConstraintAssembly assembly, QuadraticAssignmentProblem &problem)
{
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    return scoreWith(calculator, problem);
}

const DecomposedCalculator::ConstraintCosts *entryOf(const Costs &entries, const std::string &name)
{
    for (const auto &entry : entries)
    {
        if (entry.name == name)
            return &entry;
    }
    return nullptr;
}

// The raw per-unit costs of `name` in the default assembly.
std::vector<double> rawCosts(QuadraticAssignmentProblem &problem, const std::string &name)
{
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    scoreWith(calculator, problem);
    const Costs entries = calculator.constraintCosts();
    const auto *entry = entryOf(entries, name);
    return entry ? entry->unitCosts : std::vector<double>{};
}

double total(const std::vector<double> &costs)
{
    double sum = 0.0;
    for (double cost : costs)
        sum += cost;
    return sum;
}

// The shaped assembly variant (b) and (f) also run.
ConstraintAssembly shapedAssembly()
{
    return defaultConstraints()
        .exponent("locationCapacityCost", 2.0)
        .exponent("flowDistanceCost", 0.5)
        .weight("pinnedAssignmentsCost", 3)
        .soft("pinnedAssignmentsCost");
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute.
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolithic score ==\n");
    int mismatches = 0;
    int rawMismatches = 0;
    const int rounds = 500;
    std::mt19937_64 rng(99);
    std::uniform_int_distribution<int> kind(0, 4);
    for (int round = 0; round < rounds; ++round)
    {
        const int n = 1 + round % 12;
        QuadraticAssignmentProblem p = randomInstance(n, 1000 + round);
        std::vector<int> perm = randomPermutation(n, rng);
        const int what = kind(rng);
        if (what == 3)
            perm[rng() % n] = -1;  // a partial assignment
        place(p, perm);
        if (what == 4 && n > 1)
            p.facilities[0].location = p.facilities[1].location;  // two facilities on one location
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        const Score plugin = scoreWith(calculator, p);
        const Score monolithic = ScoreCalculator::calculateScore(p);
        if (!(plugin == monolithic) && ++mismatches <= 3)
        {
            std::printf("  MISMATCH @round %d: plugin=%s monolithic=%s\n", round, plugin.toString().c_str(),
                        monolithic.toString().c_str());
        }
        const ScoreCalculator::RawCosts raw = ScoreCalculator::rawCosts(p);
        const Costs entries = calculator.constraintCosts();
        const std::vector<double> expected{static_cast<double>(raw.pinnedAssignmentsCost),
                                           static_cast<double>(raw.forbiddenAssignmentsCost),
                                           static_cast<double>(raw.locationCapacityCost),
                                           static_cast<double>(raw.assignmentCost),
                                           static_cast<double>(raw.flowDistanceCost)};
        for (std::size_t c = 0; c < entries.size(); ++c)
        {
            if (total(entries[c].unitCosts) != expected[c])
                ++rawMismatches;
        }
    }
    check(mismatches == 0, std::to_string(rounds) + " random instances score identically (bit for bit)");
    check(rawMismatches == 0, "per-constraint raw totals == ScoreCalculator::rawCosts");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental == full recompute across a random walk.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    int mismatches = 0;
    int rawMismatches = 0;
    int checks = 0;
    for (int round = 0; round < 3; ++round)
    {
        const bool shaped = round == 2;
        QuadraticAssignmentProblem p = randomInstance(12, 7 + round);
        std::mt19937_64 rng(123 + round);
        place(p, randomPermutation(12, rng));
        ScoreCalculator::prepare(p);
        DecomposedCalculator calculator;
        (shaped ? shapedAssembly() : defaultConstraints()).into(calculator);
        optimaru::ScoreDirector<QuadraticAssignmentProblem> director(p, calculator);
        std::uniform_int_distribution<int> facility(0, 11);
        std::uniform_int_distribution<int> kind(0, 9);
        for (int step = 0; step < 1000; ++step)
        {
            const int a = facility(rng);
            const int b = facility(rng);
            const int what = kind(rng);
            const bool accept = (step % 3) != 0;
            const QuadraticAssignmentProblem before = p;
            director.beginMoveEvaluation();
            if (what < 7)
            {
                const int la = p.facilities[a].location;
                const int lb = p.facilities[b].location;
                director.beginVariableChanges();
                director.changeVariable(a, lb);
                director.changeVariable(b, la);
                director.endVariableChanges();
            }
            else if (what < 9)
            {
                director.changeVariable(a, p.facilities[a].location < 0 ? facility(rng) : -1);
            }
            else
            {
                director.changeVariable(a, facility(rng));
            }
            director.calculateScore();
            if (!accept)
            {
                // Undo like the engine: the inverse changes inside a rollback.
                director.beginMoveRollback();
                director.beginVariableChanges();
                for (int f = 0; f < p.facilityCount(); ++f)
                {
                    if (p.facilities[f].location != before.facilities[f].location)
                        director.changeVariable(f, before.facilities[f].location);
                }
                director.endVariableChanges();
            }
            director.endMoveEvaluation(accept);
            const Score incremental = director.calculateScore();
            const Costs live = calculator.constraintCosts();
            DecomposedCalculator fresh;
            (shaped ? shapedAssembly() : defaultConstraints()).into(fresh);
            QuadraticAssignmentProblem copy = p;
            const Score full = scoreWith(fresh, copy);
            const Costs reset = fresh.constraintCosts();
            ++checks;
            if (!sameScore(incremental, full) && ++mismatches <= 3)
            {
                std::printf("  MISMATCH round %d step %d: incremental=%s full=%s\n", round, step,
                            incremental.toString().c_str(), full.toString().c_str());
            }
            for (std::size_t c = 0; c < live.size(); ++c)
            {
                if (live[c].unitCosts != reset[c].unitCosts)
                    ++rawMismatches;
            }
        }
    }
    check(mismatches == 0, std::to_string(checks) + " moves (accepted and undone): incremental score == full");
    check(rawMismatches == 0, "raw costs after every move == a fresh reset");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Removing one component removes exactly its term.
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Partial assemblies ==\n");
    QuadraticAssignmentProblem p = sideInstance();
    identity(p);  // raw 1 / 1 / 2 / 15 / 22
    struct Case
    {
        const char *name;
        Score expected;
    };
    const std::vector<Case> cases{{"pinnedAssignmentsCost", {-3, -37.0}},
                                  {"forbiddenAssignmentsCost", {-3, -37.0}},
                                  {"locationCapacityCost", {-2, -37.0}},
                                  {"assignmentCost", {-4, -22.0}},
                                  {"flowDistanceCost", {-4, -15.0}}};
    for (const Case &c : cases)
    {
        check(sameScore(scoreWith(defaultConstraints().drop(c.name), p), c.expected),
              std::string("without ") + c.name + ": " + c.expected.toString());
    }
    check(sameScore(scoreWith(defaultConstraints().only({"flowDistanceCost"}), p), {0, -22.0}),
          "only flowDistanceCost: 0hard/-22soft");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Weight / level / exponent overrides.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Overrides ==\n");
    QuadraticAssignmentProblem p = sideInstance();
    identity(p);  // raw 1 / 1 / 2 / 15 / 22, default -4hard/-37soft
    check(sameScore(scoreWith(defaultConstraints().weight("flowDistanceCost", 3), p), {-4, -(15.0 + 66.0)}),
          "flow weight 3: -4hard/-81soft");
    check(sameScore(scoreWith(defaultConstraints().weight("locationCapacityCost", 5), p), {-12, -37.0}),
          "capacity weight 5: -12hard");
    check(sameScore(scoreWith(defaultConstraints().soft("pinnedAssignmentsCost"), p), {-3, -38.0}),
          "pinned demoted to soft: 1 soft per broken pin");
    check(sameScore(scoreWith(defaultConstraints().hard("assignmentCost"), p), {-19, -22.0}),
          "assignment cost promoted to hard: -19hard");
    // A unit exponent shapes each location: capacity [2, 0] ** 2 = 4.
    check(sameScore(scoreWith(defaultConstraints().exponent("locationCapacityCost", 2.0), p), {-6, -37.0}),
          "capacity exponent 2 shapes per location: 2^2 = 4");
    // A global exponent shapes the total: 22 ** 2.
    check(sameScore(scoreWith(defaultConstraints().exponent("flowDistanceCost", 2.0), p), {-4, -(15.0 + 484.0)}),
          "flow exponent 2 shapes the total: 22^2 = 484");
    // Sign-preserving shaping of a negative total.
    QuadraticAssignmentProblem n = emptyInstance(2);
    n.assignmentCosts = std::vector<std::vector<std::int64_t>>{{0, -9}, {2, 0}};
    place(n, {1, 0});  // direct -9 + 2 = -7
    check(sameScore(scoreWith(defaultConstraints().exponent("assignmentCost", 2.0), n), {0, 49.0}),
          "a negative total keeps its sign under an exponent: -(-7^2) soft = +49");
    // Fractional hard weights round per unit (here: per location).
    QuadraticAssignmentProblem twoPins = emptyInstance(2);
    twoPins.fixedAssignments.push_back({0, 1});
    twoPins.fixedAssignments.push_back({1, 0});
    identity(twoPins);  // both pins broken: [1, 1]
    check(sameScore(scoreWith(defaultConstraints().weight("pinnedAssignmentsCost", 0.5), twoPins), {-2, 0.0}),
          "hard weight 0.5 on two broken pins rounds per location: 2 (0.1.x rounded the total: 1)");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals() / constraintCosts().
// -------------------------------------------------------------------------
void verifyTotalsAndLayout()
{
    std::printf("== (e) Totals and cost layout ==\n");
    QuadraticAssignmentProblem p = randomInstance(9, 42);
    std::mt19937_64 rng(5);
    place(p, randomPermutation(9, rng));
    DecomposedCalculator calculator;
    defaultConstraints().weight("flowDistanceCost", 2).exponent("locationCapacityCost", 1.5).into(calculator);
    const Score score = scoreWith(calculator, p);
    double hard = 0.0;
    double soft = 0.0;
    for (const auto &entry : calculator.constraintTotals())
        (entry.isHard ? hard : soft) += entry.cost;
    check(static_cast<std::int64_t>(-hard) == score.hard && std::fabs(-soft - score.soft) < 1e-9,
          "constraintTotals() sum to the score");
    const Costs entries = calculator.constraintCosts();
    bool layout = entries.size() == 5;
    for (std::size_t c = 0; layout && c < entries.size(); ++c)
    {
        const bool global = c >= 3;
        layout = entries[c].isGlobal == global && entries[c].unitCosts.size() == (global ? 1u : 9u);
    }
    check(layout, "units list one cost per location, globals one total");
    // Location order: the capacity list equals a by-hand per-location clamp.
    std::vector<double> expected(9, 0.0);
    for (int l = 0; l < 9; ++l)
    {
        if (!p.hasCapacity[l])
            continue;
        std::int64_t load = 0;
        for (int f = 0; f < 9; ++f)
        {
            if (p.facilities[f].location == l)
                load += p.demandOf[f];
        }
        expected[l] = static_cast<double>(std::max<std::int64_t>(0, load - p.capacityOf[l]));
    }
    check(entryOf(entries, "locationCapacityCost")->unitCosts == expected, "capacity costs are in location order");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT solve(problem, calculator).
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    for (int variant = 0; variant < 2; ++variant)
    {
        const bool shaped = variant == 1;
        QuadraticAssignmentProblem p = randomInstance(10, 55 + variant);
        DecomposedCalculator calculator;
        (shaped ? shapedAssembly() : defaultConstraints()).into(calculator);
        Solver::Config config;
        config.spentLimit = std::chrono::seconds(60);
        config.stepCountLimit = 1500;
        config.randomSeed = 3;
        config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
        bool ok = true;
        try
        {
            Solver(config).solve(p, calculator);
        }
        catch (const std::exception &error)
        {
            std::printf("  %s\n", error.what());
            ok = false;
        }
        DecomposedCalculator fresh;
        (shaped ? shapedAssembly() : defaultConstraints()).into(fresh);
        QuadraticAssignmentProblem copy = p;
        check(ok && p.isBijection() && sameScore(p.score, scoreWith(fresh, copy)),
              std::string(shaped ? "shaped" : "default")
                  + " assembly: no corruption, a bijection, score from the caller's assembly");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) Names.
// -------------------------------------------------------------------------
void verifyNames()
{
    std::printf("== (g) Registry names ==\n");
    const std::vector<std::string> names = defaultConstraints().names();
    check(names
              == std::vector<std::string>{"pinnedAssignmentsCost", "forbiddenAssignmentsCost", "locationCapacityCost",
                                          "assignmentCost", "flowDistanceCost"},
          "defaultConstraints() names, in catalog order");
    std::vector<std::string> python;
    for (const std::string &name : names)
        python.push_back(optimaru::python::pythonName(name));
    check(python
              == std::vector<std::string>{"pinned_assignments_cost", "forbidden_assignments_cost",
                                          "location_capacity_cost", "assignment_cost", "flow_distance_cost"},
          "pythonName() gives the 0.2.0 snake_case names");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) Raw costs by hand: a positive and a zero case per constraint.
// -------------------------------------------------------------------------
void verifyRawCostsByHand()
{
    std::printf("== (h) Raw costs by hand ==\n");
    {
        QuadraticAssignmentProblem p = sideInstance();
        identity(p);
        // L1 is pinned to F2 but holds F1; L2 is not pinned.
        check(rawCosts(p, "pinnedAssignmentsCost") == std::vector<double>{1.0, 0.0}, "pinned: positive [1, 0]");
        // F2 stands at L2, a forbidden pair.
        check(rawCosts(p, "forbiddenAssignmentsCost") == std::vector<double>{0.0, 1.0}, "forbidden: positive [0, 1]");
        // L1 carries F1's 5 on capacity 3; L2 carries 2 on 10.
        check(rawCosts(p, "locationCapacityCost") == std::vector<double>{2.0, 0.0}, "capacity: positive [2, 0]");
        // 7 + 8.
        check(rawCosts(p, "assignmentCost") == std::vector<double>{15.0}, "assignment: positive [15]");
        // 4*3 + 5*2.
        check(rawCosts(p, "flowDistanceCost") == std::vector<double>{22.0}, "flow: positive [22]");
        place(p, {1, 0});
        check(rawCosts(p, "pinnedAssignmentsCost") == std::vector<double>{0.0, 0.0}, "pinned: zero (pin kept)");
        check(rawCosts(p, "forbiddenAssignmentsCost") == std::vector<double>{0.0, 0.0},
              "forbidden: zero (no forbidden pair)");
        check(rawCosts(p, "locationCapacityCost") == std::vector<double>{0.0, 0.0}, "capacity: zero (2 <= 3, 5 <= 10)");
    }
    {
        QuadraticAssignmentProblem p = emptyInstance(2);
        identity(p);
        check(rawCosts(p, "assignmentCost") == std::vector<double>{0.0}, "assignment: zero (no matrix)");
        check(rawCosts(p, "flowDistanceCost") == std::vector<double>{0.0}, "flow: zero (no flow)");
    }
    {
        // A pair listed twice counts once; an empty pinned location counts.
        QuadraticAssignmentProblem p = emptyInstance(3);
        p.forbiddenAssignments = {{0, 2}, {0, 2}};
        p.fixedAssignments.push_back({1, 1});
        place(p, {1, -1, 0});
        check(rawCosts(p, "forbiddenAssignmentsCost") == std::vector<double>{0.0, 0.0, 1.0},
              "forbidden: a repeated row counts once");
        check(rawCosts(p, "pinnedAssignmentsCost") == std::vector<double>{0.0, 1.0, 0.0},
              "pinned: the empty pinned location counts 1");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) The catalog and the knob guards.
// -------------------------------------------------------------------------
template <typename Component>
bool rowIs(const optimaru::ConstraintInfo &row, const char *name, optimaru::Level level, double weight, bool global)
{
    return row.name == name && row.defaultLevel == Component::kDefaultLevel
           && row.defaultWeight == Component::kDefaultWeight && row.defaultLevel == level
           && row.defaultWeight == weight && row.levelFlippable && !row.reward && row.isGlobal == global
           && row.defaultEnabled;
}

bool throwsInvalid(const std::function<void()> &edit)
{
    try
    {
        edit();
    }
    catch (const std::invalid_argument &)
    {
        return true;
    }
    return false;
}

void verifyCatalog()
{
    std::printf("== (i) Catalog ==\n");
    const auto rows = defaultConstraints().info();
    using optimaru::Level;
    check(rows.size() == 5 && rowIs<PinnedAssignmentsCost>(rows[0], "pinnedAssignmentsCost", Level::Hard, 1.0, false)
              && rowIs<ForbiddenAssignmentsCost>(rows[1], "forbiddenAssignmentsCost", Level::Hard, 1.0, false)
              && rowIs<LocationCapacityCost>(rows[2], "locationCapacityCost", Level::Hard, 1.0, false)
              && rowIs<AssignmentCost>(rows[3], "assignmentCost", Level::Soft, 1.0, true)
              && rowIs<FlowDistanceCost>(rows[4], "flowDistanceCost", Level::Soft, 1.0, true),
          "info() == the components' constants (every constraint flippable, no reward)");
    check(throwsInvalid([] { defaultConstraints().weight("flowDistanceCost", -1); }), "a negative weight throws");
    check(throwsInvalid([] { defaultConstraints().exponent("assignmentCost", 0); }), "a zero exponent throws");
    check(!throwsInvalid([] { defaultConstraints().soft("pinnedAssignmentsCost").hard("flowDistanceCost"); }),
          "every constraint flips both ways");
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyAssemblyParity();
    verifyIncremental();
    verifyPartialAssemblies();
    verifyOverrides();
    verifyTotalsAndLayout();
    verifyFullAssertSolve();
    verifyNames();
    verifyRawCostsByHand();
    verifyCatalog();
    if (failures != 0)
    {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}

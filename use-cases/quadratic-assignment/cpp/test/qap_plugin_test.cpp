// Plugin-assembly (qap/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 500 random
//       permutations and on partial / doubly-occupied assignments
//   (b) incremental score == full recompute across 3000 random moves driven
//       through the engine's ScoreDirector protocol (delta units and the
//       member-change globals), the per-constraint raw costs stay equal to a
//       fresh reset, and every constraint is non-zero on some step
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
//   (j) repeated components and the offset / clip knobs: a hand-calculated
//       repeat, a repeat on its own level (soft next to the hard default),
//       threshold and literal offsets by hand per location and on the
//       totals, the scoped side constraints charging only their evaluated
//       locations ("units"), the literal formula refused on hard at reset
//       and kept on soft, incremental == a fresh recompute over a random
//       walk (accepted and undone moves) and a FULL_ASSERT solve with
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
    // Steps on which each constraint had a non-zero raw cost (any unit):
    // every constraint must take part in the walk, or its incremental path
    // goes unchecked.
    const std::vector<std::string> names = defaultConstraints().names();
    std::vector<int> nonZero(names.size(), 0);
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
            if (live.size() != names.size() || reset.size() != names.size())
                ++rawMismatches;
            for (std::size_t c = 0; c < names.size() && c < live.size() && c < reset.size(); ++c)
            {
                const auto &costs = reset[c].unitCosts;
                if (live[c].name != names[c] || reset[c].name != names[c] || live[c].unitCosts != costs)
                    ++rawMismatches;
                if (std::any_of(costs.begin(), costs.end(), [](double cost) { return cost != 0.0; }))
                    ++nonZero[c];
            }
        }
    }
    check(mismatches == 0, std::to_string(checks) + " moves (accepted and undone): incremental score == full");
    check(rawMismatches == 0, "per-constraint raw costs after every move == a fresh reset");
    for (std::size_t k = 0; k < names.size(); ++k)
        check(nonZero[k] > 0, names[k] + " non-zero on " + std::to_string(nonZero[k]) + " walk steps");
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
        check(ok && p.isBijection() && p.score && sameScore(*p.score, scoreWith(fresh, copy)),
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

// -------------------------------------------------------------------------
// (j) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with addInstances(), as the Python bindings
// build it from a pick list. The three side constraints shape each location
// they evaluate (their scopedUnits()), the two objectives their total.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string &name)
{
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

// The side instance at the identity: pinned [1, 0] (scope L1), forbidden
// [0, 1] (scope L2), capacity [2, 0] (scope L1, L2), assignment 15, flow 22.
QuadraticAssignmentProblem sideIdentity()
{
    QuadraticAssignmentProblem p = sideInstance();
    identity(p);
    return p;
}

// Three locations, one of each scope: L1 pinned to F1, (F3, L2) forbidden,
// only L2 capacitated (10). At the identity every side constraint is 0.
QuadraticAssignmentProblem scopedInstance()
{
    QuadraticAssignmentProblem p = threeFacilities();
    p.fixedAssignments.push_back({0, 0});
    p.forbiddenAssignments.push_back({2, 1});
    p.locations[1].capacity = 10;
    identity(p);
    return p;
}

Score scoreOf(ConstraintAssembly assembly, QuadraticAssignmentProblem p)
{
    return scoreWith(std::move(assembly), p);
}

// Every kind repeated or shifted at least once: the two delta units, the
// full re-cost unit, both member-change globals; literal and threshold,
// negative offsets, a fractional hard weight; and repeats on the other level
// than their default: the flow promoted to hard (threshold), the capacity,
// pinned and forbidden rows on soft (literal, over their scope). Hard
// refuses the literal formula with an offset, so the literal copies of the
// hard side constraints sit on soft, next to a hard threshold copy.
ConstraintAssembly repeatedAssembly()
{
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("locationCapacityCost", 1.0);
    assembly.offset("flowDistanceCost", 50.0);
    assembly.addInstances(std::move(copyOf("locationCapacityCost")
                                        .soft("locationCapacityCost")
                                        .offset("locationCapacityCost", 1.0)
                                        .clip("locationCapacityCost", false)));
    assembly.addInstances(std::move(
        copyOf("pinnedAssignmentsCost").weight("pinnedAssignmentsCost", 1.5).offset("pinnedAssignmentsCost", 0.5)));
    assembly.addInstances(std::move(copyOf("pinnedAssignmentsCost")
                                        .soft("pinnedAssignmentsCost")
                                        .weight("pinnedAssignmentsCost", 1.5)
                                        .offset("pinnedAssignmentsCost", 0.5)
                                        .clip("pinnedAssignmentsCost", false)));
    assembly.addInstances(std::move(copyOf("forbiddenAssignmentsCost")
                                        .exponent("forbiddenAssignmentsCost", 2.0)
                                        .offset("forbiddenAssignmentsCost", -1.0)));
    assembly.addInstances(std::move(copyOf("locationCapacityCost")
                                        .weight("locationCapacityCost", 2.0)
                                        .exponent("locationCapacityCost", 2.0)
                                        .offset("locationCapacityCost", 2.0)));
    assembly.addInstances(std::move(copyOf("assignmentCost")
                                        .weight("assignmentCost", 0.5)
                                        .offset("assignmentCost", -20.0)
                                        .clip("assignmentCost", false)));
    assembly.addInstances(std::move(copyOf("flowDistanceCost")
                                        .weight("flowDistanceCost", 0.25)
                                        .exponent("flowDistanceCost", 1.5)
                                        .offset("flowDistanceCost", 100.0)
                                        .hard("flowDistanceCost")));
    assembly.addInstances(std::move(copyOf("flowDistanceCost")
                                        .weight("flowDistanceCost", 0.25)
                                        .exponent("flowDistanceCost", 1.5)
                                        .offset("flowDistanceCost", 100.0)
                                        .clip("flowDistanceCost", false)));
    assembly.addInstances(std::move(copyOf("forbiddenAssignmentsCost")
                                        .soft("forbiddenAssignmentsCost")
                                        .weight("forbiddenAssignmentsCost", 2.0)
                                        .offset("forbiddenAssignmentsCost", 1.0)
                                        .clip("forbiddenAssignmentsCost", false)));
    return assembly;
}

void verifyRepeatsAndOffsets()
{
    std::printf("== (j) Repeated components, offset / clip ==\n");
    {
        // flow 22 + 0.5 x 22^2 = 264.
        ConstraintAssembly assembly = copyOf("flowDistanceCost");
        assembly.addInstances(std::move(
            copyOf("flowDistanceCost").weight("flowDistanceCost", 0.5).exponent("flowDistanceCost", 2.0)));
        QuadraticAssignmentProblem p = sideIdentity();
        DecomposedCalculator calculator;
        assembly.into(calculator);
        const Score score = scoreWith(calculator, p);
        check(score.hard == 0 && score.soft == -264.0, "flow + 0.5 x flow^2: -(22 + 242) soft");
        const Costs entries = calculator.constraintCosts();
        check(entries.size() == 2 && entries[0].unitCosts == entries[1].unitCosts
                  && entries[0].unitCosts == std::vector<double>{22.0} && entries[1].weight == 0.5
                  && entries[1].exponent == 2.0 && entries[1].defaultWeight == 1.0
                  && entries[0].name == entries[1].name,
              "two instances, the same raw total, each with its own knobs");
    }
    {
        // Each instance on its own level: the hard capacity [2, 0] next to a
        // soft repeat, 3 x (2 - 1) beyond its offset on L1.
        ConstraintAssembly assembly = copyOf("locationCapacityCost");
        assembly.addInstances(std::move(copyOf("locationCapacityCost")
                                            .soft("locationCapacityCost")
                                            .weight("locationCapacityCost", 3.0)
                                            .offset("locationCapacityCost", 1.0)));
        QuadraticAssignmentProblem p = sideIdentity();
        DecomposedCalculator calculator;
        assembly.into(calculator);
        check(sameScore(scoreWith(calculator, p), {-2, -3.0}),
              "hard capacity + a soft repeat beyond 1: -2 hard / -3 soft");
        const Costs entries = calculator.constraintCosts();
        check(entries.size() == 2 && entries[0].level == optimaru::Level::Hard
                  && entries[1].level == optimaru::Level::Soft && entries[1].defaultLevel == optimaru::Level::Hard,
              "the repeat reports its own level, the default stays hard");
    }
    {
        ConstraintAssembly assembly = copyOf("locationCapacityCost");
        assembly.addInstances(copyOf("locationCapacityCost"));
        check(scoreOf(std::move(assembly), sideIdentity()).hard == -4, "capacity picked twice: 2 x 2");
    }
    {
        const QuadraticAssignmentProblem p = sideIdentity();
        check(scoreOf(copyOf("locationCapacityCost").offset("locationCapacityCost", 1.0), p).hard == -1,
              "capacity threshold offset 1 on [2, 0]: 1 + 0");
        // The three side constraints are hard by default, which refuses the
        // literal formula with an offset, so its numbers are checked on soft
        // (not rounded).
        check(sameScore(scoreOf(copyOf("locationCapacityCost")
                                    .soft("locationCapacityCost")
                                    .offset("locationCapacityCost", 1.0)
                                    .clip("locationCapacityCost", false),
                                p),
                        {0, 0.0}),
              "soft capacity literal offset 1 on [2, 0]: (2 - 1) + (0 - 1), both locations in scope");
        check(sameScore(scoreOf(copyOf("locationCapacityCost").offset("locationCapacityCost", 1.5), p), {-1, 0.0}),
              "a hard threshold offset rounds per location: 2 - 1.5 = 0.5 -> 1, L2 stays 0");
        check(sameScore(scoreOf(copyOf("locationCapacityCost")
                                    .soft("locationCapacityCost")
                                    .offset("locationCapacityCost", 2.5)
                                    .clip("locationCapacityCost", false),
                                p),
                        {0, 3.0}),
              "a soft literal offset is not rounded: (2 - 2.5) + (0 - 2.5) = -3, +3");
        check(sameScore(scoreOf(copyOf("pinnedAssignmentsCost")
                                    .soft("pinnedAssignmentsCost")
                                    .offset("pinnedAssignmentsCost", 1.0)
                                    .clip("pinnedAssignmentsCost", false),
                                p),
                        {0, 0.0}),
              "soft pinned literal offset 1 on [1, 0]: only L1 is evaluated, 1 - 1 = 0 (L2 is not charged)");
        check(scoreOf(copyOf("pinnedAssignmentsCost").offset("pinnedAssignmentsCost", -1.0), p).hard == -2,
              "pinned threshold offset -1: the broken pin costs 1 + 1");
        check(sameScore(scoreOf(copyOf("pinnedAssignmentsCost")
                                    .soft("pinnedAssignmentsCost")
                                    .offset("pinnedAssignmentsCost", -1.0)
                                    .clip("pinnedAssignmentsCost", false),
                                p),
                        {0, -2.0}),
              "soft pinned literal offset -1: L1 only, 1 + 1 (L2 is outside the scope)");
        check(scoreOf(copyOf("flowDistanceCost").offset("flowDistanceCost", 20.0), p).soft == -2.0,
              "flow threshold offset 20 on 22: -2 soft");
        check(scoreOf(copyOf("flowDistanceCost").offset("flowDistanceCost", 30.0).clip("flowDistanceCost", false), p)
                      .soft
                  == 8.0,
              "flow literal offset 30 on 22: shaped once, 22 - 30 = -8, +8 soft");
        check(scoreOf(copyOf("assignmentCost").offset("assignmentCost", -5.0), p).soft == -20.0,
              "assignment threshold offset -5 on 15: -20 soft");
    }
    {
        // The scoped side constraints on an all-zero solution: a literal
        // offset charges (#evaluated locations) x -offset, not 3 x (on soft:
        // hard refuses it).
        const QuadraticAssignmentProblem p = scopedInstance();
        check(scoreOf(defaultConstraints().only({"pinnedAssignmentsCost", "forbiddenAssignmentsCost",
                                                 "locationCapacityCost"}),
                      p)
                      .hard
                  == 0,
              "the scoped instance at the identity: every side constraint is 0");
        for (const char *name : {"pinnedAssignmentsCost", "forbiddenAssignmentsCost", "locationCapacityCost"})
        {
            check(sameScore(scoreOf(copyOf(name).soft(name).offset(name, 2.0).clip(name, false), p), {0, 2.0}),
                  std::string(name) + " soft literal offset 2: one evaluated location, +2 (not +6)");
            check(scoreOf(copyOf(name).offset(name, 2.0), p).hard == 0,
                  std::string(name) + " threshold offset 2: 0");
        }
        DecomposedCalculator calculator;
        defaultConstraints().into(calculator);
        QuadraticAssignmentProblem q = p;
        scoreWith(calculator, q);
        const Costs entries = calculator.constraintCosts();
        check(entries.size() == 5 && entries[0].scoped && entries[0].units == std::vector<int>{0}
                  && entries[1].scoped && entries[1].units == std::vector<int>{1} && entries[2].scoped
                  && entries[2].units == std::vector<int>{1} && !entries[3].scoped && !entries[4].scoped
                  && entries[0].unitCosts.size() == 3,
              "constraintCosts(): pinned units [0], forbidden [1], capacity [1]; the objectives unscoped");
    }
    {
        // On hard the literal formula with an offset is refused at reset,
        // whichever sign and setter order, for a hard default side
        // constraint and for the soft flow moved to hard: +2 on capacity
        // [2, 0] would score L1's overload as 0 and reward L2's slack (+2
        // hard), -1 would charge locations without a violation.
        auto refusal = [](const std::string &name) {
            return "Constraint '" + name
                   + "' cannot use clip=false with a non-zero offset on a level that counts "
                     "toward the hard score (hard 0 must mean no violation)";
        };
        auto resetRefusal = [](ConstraintAssembly assembly) {
            try
            {
                scoreOf(std::move(assembly), sideIdentity());
            }
            catch (const std::logic_error &error)
            {
                return std::string(error.what());
            }
            return std::string();
        };
        check(resetRefusal(copyOf("locationCapacityCost")
                               .offset("locationCapacityCost", 2.0)
                               .clip("locationCapacityCost", false))
                      == refusal("locationCapacityCost")
                  && resetRefusal(copyOf("locationCapacityCost")
                                      .clip("locationCapacityCost", false)
                                      .offset("locationCapacityCost", -1.0))
                         == refusal("locationCapacityCost"),
              "capacity literal offset +2 / -1 on its default hard: refused at reset");
        check(resetRefusal(copyOf("flowDistanceCost")
                               .offset("flowDistanceCost", 30.0)
                               .clip("flowDistanceCost", false)
                               .hard("flowDistanceCost"))
                  == refusal("flowDistanceCost"),
              "flow literal offset moved to hard afterwards: refused at reset");
        ConstraintAssembly repeated = copyOf("pinnedAssignmentsCost");
        repeated.addInstances(std::move(
            copyOf("pinnedAssignmentsCost").offset("pinnedAssignmentsCost", 1.0).clip("pinnedAssignmentsCost", false)));
        check(resetRefusal(std::move(repeated)) == refusal("pinnedAssignmentsCost"),
              "a hard literal repeat next to the default: refused at reset");
        check(resetRefusal(copyOf("locationCapacityCost").offset("locationCapacityCost", 2.0)).empty()
                  && resetRefusal(copyOf("locationCapacityCost").clip("locationCapacityCost", false)).empty()
                  && resetRefusal(copyOf("locationCapacityCost")
                                      .offset("locationCapacityCost", 2.0)
                                      .clip("locationCapacityCost", false)
                                      .soft("locationCapacityCost"))
                         .empty(),
              "capacity: a threshold offset or clip=false with offset 0 on hard, and the literal "
              "offset on soft, are allowed");
    }
    {
        // Random walk: incremental == a fresh calculator of the same assembly.
        int mismatches = 0;
        int checks = 0;
        double drift = 0.0;
        for (int round = 0; round < 3; ++round)
        {
            QuadraticAssignmentProblem p = randomInstance(12, 17 + round);
            std::mt19937_64 rng(321 + round);
            place(p, randomPermutation(12, rng));
            ScoreCalculator::prepare(p);
            DecomposedCalculator calculator;
            repeatedAssembly().into(calculator);
            optimaru::ScoreDirector<QuadraticAssignmentProblem> director(p, calculator);
            std::uniform_int_distribution<int> facility(0, 11);
            std::uniform_int_distribution<int> kind(0, 9);
            for (int step = 0; step < 1000; ++step, ++checks)
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
                DecomposedCalculator fresh;
                repeatedAssembly().into(fresh);
                QuadraticAssignmentProblem copy = p;
                const Score full = scoreWith(fresh, copy);
                drift = std::max(drift, std::fabs(full.soft - incremental.soft));
                if (!optimaru::scoresAgree(incremental, full) && ++mismatches <= 3)
                {
                    std::printf("  MISMATCH round %d step %d: incremental=%s full=%s\n", round, step,
                                incremental.toString().c_str(), full.toString().c_str());
                }
            }
        }
        std::printf("    largest soft drift %.3g\n", drift);
        check(mismatches == 0, std::to_string(checks)
                                   + " moves (accepted and undone) with repeated, shifted components: "
                                     "incremental == fresh");
    }
    {
        QuadraticAssignmentProblem p = randomInstance(10, 57);
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
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
        repeatedAssembly().into(fresh);
        QuadraticAssignmentProblem copy = p;
        check(ok && p.isBijection() && p.score && optimaru::scoresAgree(*p.score, scoreWith(fresh, copy)),
              "FULL_ASSERT solve with repeated, shifted components: no corruption, a bijection, the "
              "score == a fresh recompute");
    }
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
    verifyRepeatsAndOffsets();
    if (failures != 0)
    {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}

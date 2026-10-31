// Constraint levels, the medium fold and the knob guards (0.2.0 contract),
// engine test/ style: check() + a non-zero exit on any failure.
//   (a) Level metadata, the isHard transition shim and the bool constructor
//   (b) the medium fold: IntoSoft and BelowHard fold the same three
//       accumulators the way the domain's Score adapter does
//   (c) hard()/soft() on a medium constraint, explicit medium weights
//   (d) guards: reward never hard, non-flippable keeps its level, weight and
//       exponent validation, medium without a fold, hard reward at reset
//   (e) ConstraintAssembly::info(): the catalog read off the defaults
//   (f) exponent: per unit for unit constraints, on the total for globals
//   (g) raw costs: zero = no violation; incremental == fresh recompute on
//       a random walk for both folds (the medium cells' save/restore too)
//   (h) pythonName(): registry name -> Python name; instanceKeys()
//   (i) knob setters on a bare component, labels, wrappers that copy their
//       inner component's metadata, catalogInfo() for a catalog wider than
//       the defaults
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/python/names.hpp"

using optimaru::HardSoftScore;
using optimaru::Level;
using optimaru::MediumFold;

namespace {

int failures = 0;

void check(bool ok, const std::string& label) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}

bool same(const HardSoftScore& a, const HardSoftScore& b) {
    return a.hard == b.hard && std::fabs(a.soft - b.soft) < 1e-9;
}

std::string str(const HardSoftScore& s) { return s.toString(); }

// Three solution types over the same toy problem: four workers, six tasks,
// a 6-cycle of conflicting task pairs. They differ only in the medium fold
// their "domain" declares.
struct Tasks {
    int workerCount = 4;
    std::vector<std::vector<long>> cost{
            {7, 3, 9, 4}, {2, 8, 5, 6}, {6, 4, 3, 8}, {5, 9, 2, 7}, {8, 2, 6, 3}, {4, 6, 7, 2},
    };
    std::vector<std::pair<int, int>> conflicts{{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {0, 5}};
    std::vector<int> assignment = std::vector<int>(6, -1);
};
struct SoftFoldTasks : Tasks {};  // medium folds into soft x1000
struct HardFoldTasks : Tasks {};  // medium folds below hard, 100 per hard unit

template <typename S>
struct TasksDescriptor {
    static constexpr optimaru::VariableKind kind = optimaru::VariableKind::BASIC;
    static int entityCount(const S& s) { return static_cast<int>(s.assignment.size()); }
    static int valueCount(const S& s) { return s.workerCount; }
    static int getVariable(const S& s, int e) { return s.assignment[e]; }
    static void setVariable(S& s, int e, int v) { s.assignment[e] = v; }
};

// The domain-side declaration a migrated use case writes once, next to its
// Score adapter.
constexpr MediumFold kSoftFold = MediumFold::intoSoft(1000.0);
constexpr MediumFold kHardFold = MediumFold::belowHard(100);

}  // namespace

template <>
struct optimaru::SolutionDescriptor<Tasks> : TasksDescriptor<Tasks> {};
template <>
struct optimaru::SolutionDescriptor<SoftFoldTasks> : TasksDescriptor<SoftFoldTasks> {};
template <>
struct optimaru::SolutionDescriptor<HardFoldTasks> : TasksDescriptor<HardFoldTasks> {};
template <>
struct optimaru::MediumFoldOf<SoftFoldTasks> {
    static constexpr optimaru::MediumFold value = kSoftFold;
};
template <>
struct optimaru::MediumFoldOf<HardFoldTasks> {
    static constexpr optimaru::MediumFold value = kHardFold;
};

namespace {

// --- components, written the 0.2.0 way -------------------------------------

// Hard: conflicting task pairs sharing a worker.
template <typename S>
struct ConflictCost final : optimaru::UnitConstraint<S> {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr Level kDefaultLevel = Level::Hard;
    explicit ConflictCost(double weight = kDefaultWeight)
        : optimaru::UnitConstraint<S>("conflictCost", weight, kDefaultLevel) {}
    double cost(const S& s, int, const std::vector<int>& members) const override {
        double c = 0;
        for (std::size_t a = 0; a < members.size(); ++a)
            for (std::size_t b = a + 1; b < members.size(); ++b)
                for (auto [x, y] : s.conflicts)
                    if ((members[a] == x && members[b] == y) || (members[a] == y && members[b] == x))
                        c += 1;
        return c;
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const S& s, int, const std::vector<int>& members, int entity) const override {
        double c = 0;
        for (int t : members) {
            if (t == entity) continue;
            for (auto [x, y] : s.conflicts)
                if ((t == x && entity == y) || (t == y && entity == x)) c += 1;
        }
        return c;
    }
};

// Medium: tasks beyond two on one worker. No delta contract (the clamp), so
// it runs the full re-cost path and its medium cells go through the
// transaction snapshot.
template <typename S>
struct OverloadCost final : optimaru::UnitConstraint<S> {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr Level kDefaultLevel = Level::Medium;
    explicit OverloadCost(double weight = kDefaultWeight)
        : optimaru::UnitConstraint<S>("overloadCost", weight, kDefaultLevel) {}
    double cost(const S&, int, const std::vector<int>& members) const override {
        return members.size() > 2 ? static_cast<double>(members.size() - 2) : 0.0;
    }
};

// Medium global: workers left without a task.
template <typename S>
struct IdleWorkerCost final : optimaru::GlobalConstraint<S> {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr Level kDefaultLevel = Level::Medium;
    explicit IdleWorkerCost(double weight = kDefaultWeight)
        : optimaru::GlobalConstraint<S>("idleWorkerCost", weight, kDefaultLevel) {}
    std::vector<int> counts;
    void reset(const S&, const std::vector<std::vector<int>>& membersPerUnit) override {
        counts.clear();
        for (const auto& m : membersPerUnit) counts.push_back(static_cast<int>(m.size()));
    }
    void onUnitChanged(const S&, int unit, const std::vector<int>& members) override {
        counts[unit] = static_cast<int>(members.size());
    }
    double cost() const override {
        double idle = 0;
        for (int n : counts) idle += n == 0 ? 1.0 : 0.0;
        return idle;
    }
};

// Soft: the task cost matrix.
template <typename S>
struct TaskCostCost final : optimaru::UnitConstraint<S> {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr Level kDefaultLevel = Level::Soft;
    explicit TaskCostCost(double weight = kDefaultWeight)
        : optimaru::UnitConstraint<S>("taskCostCost", weight, kDefaultLevel) {}
    double cost(const S& s, int unit, const std::vector<int>& members) const override {
        double c = 0;
        for (int t : members) c += static_cast<double>(s.cost[t][unit]);
        return c;
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const S& s, int unit, const std::vector<int>&, int entity) const override {
        return static_cast<double>(s.cost[entity][unit]);
    }
};

// Soft reward (raw <= 0): tasks on worker 0, at a fractional default weight.
template <typename S>
struct PreferenceReward final : optimaru::UnitConstraint<S> {
    static constexpr double kDefaultWeight = 0.5;
    static constexpr Level kDefaultLevel = Level::Soft;
    explicit PreferenceReward(double weight = kDefaultWeight)
        : optimaru::UnitConstraint<S>("preferenceReward", weight, kDefaultLevel) {
        this->reward = true;
        this->levelFlippable = false;
    }
    double cost(const S&, int unit, const std::vector<int>& members) const override {
        return unit == 0 ? -static_cast<double>(members.size()) : 0.0;
    }
};

// Structural hard (not flippable): tasks on the forbidden worker 3.
template <typename S>
struct ForbiddenWorkerCost final : optimaru::UnitConstraint<S> {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr Level kDefaultLevel = Level::Hard;
    explicit ForbiddenWorkerCost(double weight = kDefaultWeight)
        : optimaru::UnitConstraint<S>("forbiddenWorkerCost", weight, kDefaultLevel) {
        this->levelFlippable = false;
    }
    double cost(const S&, int unit, const std::vector<int>& members) const override {
        return unit == 3 ? static_cast<double>(members.size()) : 0.0;
    }
};

template <typename S>
optimaru::ConstraintAssembly<S> defaults() {
    optimaru::ConstraintAssembly<S> assembly;
    assembly.template emplace<ConflictCost<S>>();
    assembly.template emplace<OverloadCost<S>>();
    assembly.template emplace<IdleWorkerCost<S>>();
    assembly.template emplace<TaskCostCost<S>>();
    assembly.template emplace<PreferenceReward<S>>();
    assembly.template emplace<ForbiddenWorkerCost<S>>();
    return assembly;
}

template <typename S>
HardSoftScore scoreOf(optimaru::ConstraintAssembly<S> assembly, S& s) {
    optimaru::DecomposedScoreCalculator<S> calc;
    assembly.into(calc);
    calc.resetWorkingSolution(s);
    return calc.calculateScore();
}

template <typename S>
bool throwsLogic(optimaru::ConstraintAssembly<S> assembly, S& s) {
    try {
        scoreOf(std::move(assembly), s);
    } catch (const std::logic_error&) {
        return true;
    }
    return false;
}

bool throwsInvalid(const std::function<void()>& edit) {
    try {
        edit();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

// Assignment A = [0,0,0,1,1,3], by hand:
//   conflictCost        worker0 {0,1,2}: (0,1),(1,2); worker1 {3,4}: (3,4) -> 3 hard
//   overloadCost        worker0 holds 3 tasks -> 1 medium
//   idleWorkerCost      worker2 idle -> 1 medium
//   taskCostCost        7 + 2 + 6 + 9 + 2 + 2 = 28 soft
//   preferenceReward    3 tasks on worker0 -> raw -3, x0.5 = -1.5 soft
//   forbiddenWorkerCost task 5 on worker3 -> 1 hard
// hard 4, medium 2, soft 26.5.
template <typename S>
S assignmentA() {
    S s;
    s.assignment = {0, 0, 0, 1, 1, 3};
    return s;
}

void testLevelMetadata() {
    std::printf("== (a) Level metadata and the isHard transition shim ==\n");
    ConflictCost<Tasks> conflict;
    check(conflict.weight == ConflictCost<Tasks>::kDefaultWeight && conflict.level == Level::Hard,
          "the default constructor carries kDefaultWeight / kDefaultLevel");
    check(conflict.isHard() && static_cast<bool>(conflict.isHard),
          "isHard() (0.2.0) and the shim's bool read agree on a hard constraint");
    check(!conflict.reward && conflict.levelFlippable, "reward=false, levelFlippable=true by default");
    conflict.isHard = false;  // the pre-0.2.0 write, through the shim
    check(conflict.level == Level::Soft && !conflict.isHard(), "shim write isHard=false sets Soft");
    conflict.isHard = true;
    check(conflict.level == Level::Hard, "shim write isHard=true sets Hard");

    // Copies rebind the shim to their own level.
    ConflictCost<Tasks> copy = conflict;
    copy.isHard = false;
    check(conflict.level == Level::Hard && copy.level == Level::Soft,
          "a copied constraint's isHard writes its own level, not the original's");
    copy.isHard = conflict.isHard;
    check(copy.level == Level::Hard, "assigning one shim from another copies the flag");

    // The pre-0.2.0 (name, weight, bool) constructor.
    struct Legacy final : optimaru::UnitConstraint<Tasks> {
        explicit Legacy(bool hard) : UnitConstraint("legacy", 2.0, hard) {}
        double cost(const Tasks&, int, const std::vector<int>&) const override { return 0.0; }
    };
    check(Legacy(true).level == Level::Hard && Legacy(false).level == Level::Soft,
          "the transition bool constructor maps true/false to Hard/Soft");
    check(std::string(optimaru::levelName(Level::Medium)) == "medium",
          "levelName spells the levels for clients");
    std::printf("\n");
}

void testMediumFold() {
    std::printf("== (b) The medium fold ==\n");
    {
        SoftFoldTasks s = assignmentA<SoftFoldTasks>();
        optimaru::DecomposedScoreCalculator<SoftFoldTasks> calc;
        defaults<SoftFoldTasks>().into(calc);
        calc.resetWorkingSolution(s);
        const HardSoftScore score = calc.calculateScore();
        check(same(score, {-4, -(26.5 + 2 * 1000.0)}),
              "IntoSoft(1000): -4hard / -(26.5 + 2 x 1000)soft, got " + str(score));
        const auto levels = calc.levelScores();
        check(levels.hard == -4 && levels.medium == -2.0 && std::fabs(levels.soft + 26.5) < 1e-9,
              "levelScores() reports the three accumulators before the fold");
        check(same(kSoftFold.toEngine(levels.hard, levels.medium, levels.soft), score),
              "the domain adapter's MediumFold::toEngine == the calculator's fold");
        bool levelsReported = true;
        for (const auto& total : calc.constraintTotals()) {
            if (total.name == "overloadCost")
                levelsReported &= total.level == Level::Medium && total.cost == 1.0 && !total.isHard;
            if (total.name == "conflictCost")
                levelsReported &= total.level == Level::Hard && total.isHard && total.cost == 3.0;
            if (total.name == "idleWorkerCost")
                levelsReported &= total.level == Level::Medium && total.cost == 1.0;
        }
        check(levelsReported, "constraintTotals() report each level, medium in medium units");
    }
    {
        HardFoldTasks s = assignmentA<HardFoldTasks>();
        const HardSoftScore score = scoreOf(defaults<HardFoldTasks>(), s);
        check(same(score, {-(4 * 100 + 2), -26.5}),
              "BelowHard(100): -(4 x 100 + 2)hard / -26.5soft, got " + str(score));
    }
    {
        Tasks s = assignmentA<Tasks>();
        check(throwsLogic(defaults<Tasks>(), s),
              "a medium constraint on a domain without a medium fold throws at reset");
        const HardSoftScore score = scoreOf(
                defaults<Tasks>().soft("overloadCost").soft("idleWorkerCost"), s);
        check(same(score, {-4, -28.5}),
              "the same constraints demoted to soft score on a fold-less domain");
    }
    std::printf("\n");
}

void testMediumKnobs() {
    std::printf("== (c) hard()/soft()/weight on a medium constraint ==\n");
    SoftFoldTasks s = assignmentA<SoftFoldTasks>();
    check(same(scoreOf(defaults<SoftFoldTasks>().hard("overloadCost"), s), {-5, -1026.5}),
          "hard() on medium: 1 hard unit per raw unit (not the fold factor)");
    check(same(scoreOf(defaults<SoftFoldTasks>().soft("overloadCost"), s), {-4, -1027.5}),
          "soft() on medium: 1 soft unit per raw unit, out of the fold");
    check(same(scoreOf(defaults<SoftFoldTasks>().weight("overloadCost", 3.0), s), {-4, -4026.5}),
          "an explicit medium weight stays in medium units (3 x 1000 per raw unit)");
    check(same(scoreOf(defaults<SoftFoldTasks>().level("taskCostCost", Level::Medium), s),
               {-4, -(-1.5 + 30 * 1000.0)}),
          "level(name, Medium) moves a soft constraint onto the medium level");
    HardFoldTasks h = assignmentA<HardFoldTasks>();
    check(same(scoreOf(defaults<HardFoldTasks>().soft("overloadCost"), h), {-401, -27.5}),
          "BelowHard: soft() takes the medium term out of the hard level");
    // The fold applies to the whole score, not only when a medium term is
    // selected (the Python base's score_costs reproduces both numbers from
    // the same raw costs: cpsat-common tests/test_constraint_base.py).
    check(same(scoreOf(defaults<HardFoldTasks>().only({"conflictCost", "forbiddenWorkerCost"}), h),
               {-400, 0.0}),
          "BelowHard with no medium constraint selected: hard x 100 all the same");
    check(same(scoreOf(defaults<HardFoldTasks>().soft("overloadCost").soft("idleWorkerCost"), h),
               {-400, -28.5}),
          "BelowHard with both medium terms demoted: hard x 100, medium raw units in soft");
    check(same(scoreOf(defaults<HardFoldTasks>().weight("overloadCost", 0.4), h), {-401, -26.5}),
          "BelowHard rounds each medium unit like hard (0.4 -> 0)");
    std::printf("\n");
}

void testGuards() {
    std::printf("== (d) Guards ==\n");
    SoftFoldTasks s = assignmentA<SoftFoldTasks>();
    check(throwsInvalid([] { defaults<SoftFoldTasks>().hard("preferenceReward"); }),
          "hard() on a reward throws");
    check(!throwsInvalid([] { defaults<SoftFoldTasks>().soft("preferenceReward"); }),
          "soft() on a soft non-flippable reward is a no-op, not an error");
    check(throwsInvalid([] { defaults<SoftFoldTasks>().soft("forbiddenWorkerCost"); }),
          "soft() on a non-flippable hard constraint throws");
    check(!throwsInvalid([] { defaults<SoftFoldTasks>().hard("forbiddenWorkerCost"); }),
          "hard() on a non-flippable hard constraint is a no-op");
    check(throwsInvalid([] {
              optimaru::ConstraintAssembly<SoftFoldTasks> assembly;
              auto reward = std::make_unique<PreferenceReward<SoftFoldTasks>>();
              reward->levelFlippable = true;  // even a flippable reward
              assembly.add(std::move(reward));
              assembly.hard("preferenceReward");
          }),
          "the reward guard holds even when the level is flippable");
    check(throwsInvalid([] { defaults<SoftFoldTasks>().weight("taskCostCost", -1.0); }),
          "a negative weight throws");
    check(throwsInvalid([] {
              defaults<SoftFoldTasks>().weight("taskCostCost",
                                               std::numeric_limits<double>::quiet_NaN());
          }),
          "a NaN weight throws");
    check(throwsInvalid([] { defaults<SoftFoldTasks>().exponent("taskCostCost", 0.0); }),
          "exponent 0 throws");
    check(throwsInvalid([] { defaults<SoftFoldTasks>().exponent("taskCostCost", -2.0); }),
          "a negative exponent throws");
    check(!throwsInvalid([] { defaults<SoftFoldTasks>().weight("taskCostCost", 0.0); }),
          "weight 0 is allowed (an opt-in objective left off)");
    {
        // Metadata written directly (bypassing the assembly) is checked at reset.
        optimaru::ConstraintAssembly<SoftFoldTasks> assembly;
        auto reward = std::make_unique<PreferenceReward<SoftFoldTasks>>();
        reward->level = Level::Hard;
        assembly.add(std::move(reward));
        check(throwsLogic(std::move(assembly), s), "a hard reward is rejected at reset");
    }
    std::printf("\n");
}

void testInfo() {
    std::printf("== (e) ConstraintAssembly::info() ==\n");
    const auto rows = defaults<SoftFoldTasks>().info();
    struct Expected {
        const char* name;
        Level level;
        double weight;
        bool flippable;
        bool reward;
        bool global;
    };
    // Units first, then globals (assembly order within each), like names().
    const std::vector<Expected> expected{
            {"conflictCost", Level::Hard, 1.0, true, false, false},
            {"overloadCost", Level::Medium, 1.0, true, false, false},
            {"taskCostCost", Level::Soft, 1.0, true, false, false},
            {"preferenceReward", Level::Soft, 0.5, false, true, false},
            {"forbiddenWorkerCost", Level::Hard, 1.0, false, false, false},
            {"idleWorkerCost", Level::Medium, 1.0, true, false, true},
    };
    bool ok = rows.size() == expected.size();
    for (std::size_t i = 0; ok && i < rows.size(); ++i) {
        ok = rows[i].name == expected[i].name && rows[i].defaultLevel == expected[i].level &&
             rows[i].defaultWeight == expected[i].weight &&
             rows[i].levelFlippable == expected[i].flippable &&
             rows[i].reward == expected[i].reward && rows[i].isGlobal == expected[i].global;
    }
    check(ok, "one row per component with its default level, weight, flippability, reward");
    const auto names = defaults<SoftFoldTasks>().names();
    bool sameOrder = names.size() == rows.size();
    for (std::size_t i = 0; sameOrder && i < rows.size(); ++i) sameOrder = names[i] == rows[i].name;
    check(sameOrder, "info() rows follow names() order");
    const auto edited = defaults<SoftFoldTasks>().weight("taskCostCost", 4.0).info();
    check(edited[2].defaultWeight == 4.0,
          "info() reads the assembly it is asked (the catalog = the DEFAULT assembly)");
    std::printf("\n");
}

void testExponent() {
    std::printf("== (f) Exponent ==\n");
    // C = [0,0,0,1,1,1]: overload 1 on worker0 and 1 on worker1; idle workers
    // 2 and 3. Per-unit shaping of the unit constraint: 1^2 + 1^2 = 2 (not
    // (1+1)^2); the global shapes its total: 2^2 = 4.
    SoftFoldTasks s;
    s.assignment = {0, 0, 0, 1, 1, 1};
    auto pair = [] {
        return defaults<SoftFoldTasks>().only({"overloadCost", "idleWorkerCost"});
    };
    check(same(scoreOf(pair(), s), {0, -4000.0}), "unshaped: medium 1 + 1 + 2 = 4");
    check(same(scoreOf(pair().exponent("overloadCost", 2.0).exponent("idleWorkerCost", 2.0), s),
               {0, -6000.0}),
          "exponent 2: per unit for the unit constraint (2), on the total for the global (4)");
    std::printf("\n");
}

template <typename S>
void randomWalk(const std::string& label) {
    S s = assignmentA<S>();
    optimaru::DecomposedScoreCalculator<S> calc;
    defaults<S>().into(calc);
    calc.resetWorkingSolution(s);
    std::mt19937_64 rng(17);
    bool agree = true;
    for (int step = 0; step < 2000; ++step) {
        const bool trial = step % 3 == 0;
        const int t = static_cast<int>(rng() % 6);
        const int old = s.assignment[t];
        const int w = (rng() % 7 == 0) ? -1 : static_cast<int>(rng() % 4);
        if (trial) calc.beginMoveEvaluation();
        calc.beforeVariableChanged(t);
        s.assignment[t] = w;
        calc.afterVariableChanged(t);
        optimaru::DecomposedScoreCalculator<S> fresh;
        defaults<S>().into(fresh);
        fresh.resetWorkingSolution(s);
        agree &= same(calc.calculateScore(), fresh.calculateScore());
        if (trial) {
            // Reject every third move: the snapshot must restore the medium
            // cells of the full re-cost constraint too.
            const HardSoftScore candidate = calc.calculateScore();
            (void)candidate;
            calc.beginMoveRollback();
            calc.beforeVariableChanged(t);
            s.assignment[t] = old;
            calc.afterVariableChanged(t);
            calc.endMoveEvaluation(false);
            optimaru::DecomposedScoreCalculator<S> restored;
            defaults<S>().into(restored);
            restored.resetWorkingSolution(s);
            agree &= same(calc.calculateScore(), restored.calculateScore());
        }
    }
    check(agree, label + ": incremental == fresh recompute over 2000 moves (1/3 rejected)");
}

void testRawCostsAndIncremental() {
    std::printf("== (g) Raw costs and incremental scoring ==\n");
    // [0,1,2,0,1,2]: no conflict pair shares a worker, nobody holds more than
    // two tasks, worker3 is empty: every penalty but idleWorkerCost is 0.
    SoftFoldTasks s;
    s.assignment = {0, 1, 2, 0, 1, 2};
    optimaru::DecomposedScoreCalculator<SoftFoldTasks> calc;
    defaults<SoftFoldTasks>().into(calc);
    calc.resetWorkingSolution(s);
    calc.calculateScore();
    bool zeros = true;
    for (const auto& entry : calc.constraintCosts()) {
        if (entry.name == "conflictCost" || entry.name == "overloadCost" ||
            entry.name == "forbiddenWorkerCost") {
            for (double raw : entry.unitCosts) zeros &= raw == 0.0;
        }
        if (entry.name == "idleWorkerCost") zeros &= entry.unitCosts == std::vector<double>{1.0};
        if (entry.name == "overloadCost") zeros &= entry.level == Level::Medium && !entry.isHard;
        if (entry.name == "preferenceReward") zeros &= entry.reward && !entry.levelFlippable;
    }
    check(zeros, "raw cost 0 = no violation; constraintCosts() carry level/reward/flippable");
    check(calc.calculateScore().hard == 0, "no hard violation -> 0hard");
    randomWalk<SoftFoldTasks>("IntoSoft");
    randomWalk<HardFoldTasks>("BelowHard");
    std::printf("\n");
}

void testPythonName() {
    std::printf("== (h) pythonName ==\n");
    using optimaru::python::pythonName;
    check(pythonName("facilityCapacityCost") == "facility_capacity_cost",
          "facilityCapacityCost -> facility_capacity_cost");
    check(pythonName("vehicleCapacityCost") == "vehicle_capacity_cost",
          "vehicleCapacityCost -> vehicle_capacity_cost");
    check(pythonName("facility setup cost") == "facility_setup_cost",
          "the pre-0.2.0 spaced spelling still maps");
    check(pythonName("distanceFromFacilityCost") == "distance_from_facility_cost",
          "distanceFromFacilityCost -> distance_from_facility_cost");
    using optimaru::python::instanceKeys;
    check(instanceKeys({"day_pattern_cost", "min_rest_cost", "day_pattern_cost", "day_pattern_cost"}) ==
                  std::vector<std::string>{"day_pattern_cost", "min_rest_cost", "day_pattern_cost#2",
                                           "day_pattern_cost#3"},
          "instanceKeys: a repeated name's k-th instance is '<name>#<k>', a single one keeps its name");
    std::printf("\n");
}

// A wrapper in the employee-scheduling ScopedUnit style: it scores its inner
// component on a subset of the units and takes over ALL of its metadata.
template <typename S>
struct OnlyWorkerZero final : optimaru::UnitConstraint<S> {
    std::unique_ptr<optimaru::UnitConstraint<S>> inner;
    explicit OnlyWorkerZero(std::unique_ptr<optimaru::UnitConstraint<S>> wrapped)
        : optimaru::UnitConstraint<S>(wrapped->name, wrapped->weight, wrapped->level),
          inner(std::move(wrapped)) {
        static_cast<optimaru::ConstraintMetadata&>(*this) = *inner;
    }
    double cost(const S& s, int unit, const std::vector<int>& members) const override {
        return unit == 0 ? inner->cost(s, unit, members) : 0.0;
    }
};

void testComponentsOutsideAnAssembly() {
    std::printf("== (i) Components outside an assembly, labels, catalogInfo ==\n");
    {
        TaskCostCost<SoftFoldTasks> c;
        check(throwsInvalid([&] { c.setWeight(-1.0); }) && c.weight == 1.0,
              "setWeight refuses a negative weight and leaves the weight alone");
        c.setWeight(2.5);
        check(c.weight == 2.5, "setWeight re-states the weight");
        check(throwsInvalid([&] { c.setExponent(0.0); }) && c.exponent == 1.0,
              "setExponent refuses 0");
        c.setLevel(Level::Hard);
        check(c.level == Level::Hard, "setLevel moves a flippable constraint");
        PreferenceReward<SoftFoldTasks> reward;
        check(throwsInvalid([&] { reward.setLevel(Level::Hard); }), "setLevel(Hard) on a reward throws");
        ForbiddenWorkerCost<SoftFoldTasks> forbidden;
        check(throwsInvalid([&] { forbidden.setLevel(Level::Soft); }),
              "setLevel on a non-flippable constraint throws");
        forbidden.setLevel(Level::Hard);
        check(forbidden.level == Level::Hard, "re-stating a non-flippable level is a no-op");
    }
    {
        // Two instances of one name with their own labels, the way a rule
        // factory adds them; the registry name stays the lookup key.
        SoftFoldTasks s = assignmentA<SoftFoldTasks>();
        optimaru::DecomposedScoreCalculator<SoftFoldTasks> calc;
        auto first = std::make_unique<TaskCostCost<SoftFoldTasks>>();
        first->label = "task cost (all workers)";
        auto second = std::make_unique<OnlyWorkerZero<SoftFoldTasks>>(
                std::make_unique<TaskCostCost<SoftFoldTasks>>(3.0));
        second->label = "task cost (worker 0)";
        calc.add(std::move(first));
        calc.add(std::move(second));
        calc.resetWorkingSolution(s);
        const HardSoftScore score = calc.calculateScore();
        check(same(score, {0, -(28.0 + 3.0 * 15.0)}), "both instances score: 28 + 3 x 15, got " + str(score));
        const auto totals = calc.constraintTotals();
        const auto costs = calc.constraintCosts();
        check(totals.size() == 2 && totals[0].name == "taskCostCost" && totals[1].name == "taskCostCost" &&
                      totals[0].label == "task cost (all workers)" && totals[1].label == "task cost (worker 0)" &&
                      totals[1].cost == 45.0,
              "constraintTotals() report each instance under its registry name with its label");
        check(costs.size() == 2 && costs[1].label == "task cost (worker 0)" && costs[1].weight == 3.0,
              "constraintCosts() carry the label and the instance's own weight");
    }
    {
        auto wrapped = std::make_unique<OnlyWorkerZero<SoftFoldTasks>>(
                std::make_unique<PreferenceReward<SoftFoldTasks>>());
        check(wrapped->reward && !wrapped->levelFlippable && wrapped->weight == 0.5 &&
                      wrapped->level == Level::Soft && wrapped->name == "preferenceReward",
              "a wrapper copying the inner metadata keeps reward / levelFlippable / weight / level");
        check(throwsInvalid([&] { wrapped->setLevel(Level::Hard); }),
              "so the reward guard still holds on the wrapper");
    }
    {
        // A catalog wider than the defaults: the defaults hold conflict,
        // taskCost (and an opt-in weight-0 overload); the catalog adds
        // forbiddenWorker and restates conflict.
        optimaru::ConstraintAssembly<SoftFoldTasks> defaultSet;
        defaultSet.emplace<ConflictCost<SoftFoldTasks>>();
        defaultSet.emplace<TaskCostCost<SoftFoldTasks>>();
        defaultSet.emplace<OverloadCost<SoftFoldTasks>>(0.0);
        optimaru::ConstraintAssembly<SoftFoldTasks> catalog;
        catalog.emplace<ConflictCost<SoftFoldTasks>>();
        catalog.emplace<ForbiddenWorkerCost<SoftFoldTasks>>();
        const auto rows = optimaru::catalogInfo(defaultSet, catalog);
        check(rows.size() == 4 && rows[0].name == "conflictCost" && rows[1].name == "taskCostCost" &&
                      rows[2].name == "overloadCost" && rows[3].name == "forbiddenWorkerCost",
              "catalogInfo: the default rows first, then the catalog-only ones, each name once");
        check(rows[0].defaultEnabled && rows[1].defaultEnabled && !rows[2].defaultEnabled &&
                      !rows[3].defaultEnabled,
              "defaultEnabled: in the defaults at weight > 0 (not an opt-in weight 0, not catalog-only)");
        check(rows[3].defaultLevel == Level::Hard && !rows[3].levelFlippable,
              "a catalog-only row carries its component's own defaults");
        optimaru::ConstraintAssembly<SoftFoldTasks> drifted;
        drifted.emplace<ConflictCost<SoftFoldTasks>>(5.0);
        bool threw = false;
        try {
            optimaru::catalogInfo(defaultSet, drifted);
        } catch (const std::logic_error&) {
            threw = true;
        }
        check(threw, "catalogInfo throws when the catalog declares a default differently");
        const auto plain = defaultSet.info();
        check(plain[0].defaultEnabled && !plain[2].defaultEnabled,
              "info() alone: defaultEnabled = weight > 0");
    }
    std::printf("\n");
}

}  // namespace

int main() {
    testLevelMetadata();
    testMediumFold();
    testMediumKnobs();
    testGuards();
    testInfo();
    testExponent();
    testRawCostsAndIncremental();
    testPythonName();
    testComponentsOutsideAnAssembly();
    if (failures == 0) {
        std::printf("All constraint-level tests PASSED.\n");
        return 0;
    }
    std::printf("%d constraint-level test(s) FAILED.\n", failures);
    return 1;
}

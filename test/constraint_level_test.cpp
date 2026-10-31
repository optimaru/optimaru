// Constraint levels, the medium fold and the knob guards (0.2.0 contract),
// engine test/ style: check() + a non-zero exit on any failure.
//   (a) Level metadata, isHard() and metadata copies
//   (b) the medium fold: IntoSoft and BelowHard fold the same three
//       accumulators the way the domain's Score adapter does
//   (c) hard()/soft() on a medium constraint, explicit medium weights
//   (d) guards: reward never hard, non-flippable keeps its level, weight and
//       exponent validation (NaN, +inf, negative), medium without a fold,
//       hard reward at reset
//   (e) ConstraintAssembly::info(): the catalog read off the defaults
//   (f) exponent: per unit for unit constraints, on the total for globals;
//       sign-preserving on a reward's negative raw cost
//   (g) raw costs: zero = no violation; incremental == fresh recompute on
//       a random walk for both folds (the medium cells' save/restore too)
//   (h) pythonName(): registry name -> Python name; instanceKeys()
//   (i) knob setters on a bare component, labels, wrappers that copy their
//       inner component's metadata, catalogInfo() for a catalog wider than
//       the defaults
//   (j) python::levelForHard(): the bindings' hard=True/False guard and its
//       wording (wantedLevel / applyKnobs raise it as ValueError)
//   (k) LIST kind with a medium level: every preview (unit cells, a
//       previewing global, BelowHard rounding) equals the applied score,
//       which FULL_ASSERT checks against a full recompute
//   (l) the range of a score: a hard total beyond int64 saturates at
//       -INT64_MAX (never wraps to a feasible-looking score), the BelowHard
//       fold saturates too, incremental == fresh across the saturation
//       boundary, and an out-of-range contribution throws overflow_error
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#include "optimaru/python/level_guard.hpp"
#include "optimaru/python/names.hpp"
#include "optimaru/testing/preview_parity.hpp"

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

// LIST kind (the shape of vehicle-routing / pickup-delivery): 24 cities on
// three routes, each city with a demand. The two types differ only in their
// medium fold, like the Tasks types above.
struct Routes {
    std::vector<std::pair<int, int>> city;  // grid coordinates per element; the depot is (0, 0)
    std::vector<int> demand;                // per element
    int capacity = 18;
    std::vector<std::vector<int>> routes;   // the list planning variables
};
struct SoftFoldRoutes : Routes {};  // medium folds into soft x1000
struct HardFoldRoutes : Routes {};  // medium folds below hard, 100 per hard unit

template <typename S>
struct RoutesDescriptor {
    static constexpr optimaru::VariableKind kind = optimaru::VariableKind::LIST;
    static int listCount(const S& s) { return static_cast<int>(s.routes.size()); }
    static int elementCount(const S& s) { return static_cast<int>(s.city.size()); }
    static int listSize(const S& s, int l) { return static_cast<int>(s.routes[l].size()); }
    static int getElement(const S& s, int l, int p) { return s.routes[l][p]; }
    static void insertElement(S& s, int l, int p, int e) {
        s.routes[l].insert(s.routes[l].begin() + p, e);
    }
    static int removeElement(S& s, int l, int p) {
        const int e = s.routes[l][p];
        s.routes[l].erase(s.routes[l].begin() + p);
        return e;
    }
    static void reverseSegment(S& s, int l, int first, int last) {
        std::reverse(s.routes[l].begin() + first, s.routes[l].begin() + last + 1);
    }
};

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
template <>
struct optimaru::SolutionDescriptor<SoftFoldRoutes> : RoutesDescriptor<SoftFoldRoutes> {};
template <>
struct optimaru::SolutionDescriptor<HardFoldRoutes> : RoutesDescriptor<HardFoldRoutes> {};
template <>
struct optimaru::MediumFoldOf<SoftFoldRoutes> {
    static constexpr optimaru::MediumFold value = kSoftFold;
};
template <>
struct optimaru::MediumFoldOf<HardFoldRoutes> {
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
    std::printf("== (a) Level metadata ==\n");
    ConflictCost<Tasks> conflict;
    check(conflict.weight == ConflictCost<Tasks>::kDefaultWeight && conflict.level == Level::Hard,
          "the default constructor carries kDefaultWeight / kDefaultLevel");
    check(conflict.isHard(), "isHard() reads level == Hard");
    check(!conflict.reward && conflict.levelFlippable, "reward=false, levelFlippable=true by default");
    conflict.setLevel(Level::Soft);
    check(conflict.level == Level::Soft && !conflict.isHard(), "setLevel(Soft) clears isHard()");
    conflict.setLevel(Level::Hard);
    check(conflict.isHard(), "setLevel(Hard) sets isHard()");

    // A copy owns its own metadata.
    ConflictCost<Tasks> copy = conflict;
    copy.setLevel(Level::Soft);
    check(conflict.level == Level::Hard && copy.level == Level::Soft,
          "changing a copied constraint's level leaves the original alone");
    copy = conflict;
    check(copy.isHard(), "copy assignment copies the level");
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
    check(throwsInvalid([] {
              defaults<SoftFoldTasks>().weight("taskCostCost",
                                               std::numeric_limits<double>::infinity());
          }),
          "a +inf weight throws");
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
    // A reward keeps its sign: on A, preferenceReward's raw -3 (worker 0)
    // shapes to -9 (not +9), x0.5 = -4.5, so soft 28 - 4.5 = 23.5 (+ the fold).
    SoftFoldTasks a = assignmentA<SoftFoldTasks>();
    check(std::fabs(optimaru::shapeCost(-3.0, 2.0) + 9.0) < 1e-12 &&
                  std::fabs(optimaru::shapeCost(-4.0, 0.5) + 2.0) < 1e-12,
          "shapeCost raises the magnitude of a negative raw cost and keeps its sign");
    check(same(scoreOf(defaults<SoftFoldTasks>().exponent("preferenceReward", 2.0), a),
               {-4, -(23.5 + 2 * 1000.0)}),
          "exponent 2 on a reward: raw -3 -> -9, x0.5 = -4.5 soft (a larger reward, not a penalty)");
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

// --- (j) the bindings' level guard -----------------------------------------

std::string refusal(const std::function<void()>& edit) {
    try {
        edit();
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    return "";
}

void testBindingLevelGuard() {
    std::printf("== (j) python::levelForHard: the bindings' hard=True/False guard ==\n");
    using optimaru::python::levelForHard;
    // What solve(..., constraints=[RewardCost(hard=True)]) and
    // StructuralCost(hard=False) raise as ValueError through wantedLevel /
    // applyKnobs, in the Python base's words.
    const PreferenceReward<SoftFoldTasks> reward;
    check(refusal([&] { levelForHard(reward, true, "'preference_reward'"); }) ==
                  "constraint 'preference_reward' is a reward (raw cost <= 0) and cannot be hard",
          "hard=True on a reward is refused: 'is a reward'");
    check(levelForHard(reward, false, "'preference_reward'") == Level::Soft,
          "hard=False on a soft reward re-states its level");
    const ForbiddenWorkerCost<SoftFoldTasks> forbidden;
    check(refusal([&] { levelForHard(forbidden, false, "'forbidden_worker_cost'"); }) ==
                  "constraint 'forbidden_worker_cost' is always hard; hard=False is not allowed",
          "hard=False on a non-flippable hard constraint is refused: 'is always hard'");
    check(levelForHard(forbidden, true, "'forbidden_worker_cost'") == Level::Hard,
          "hard=True on it re-states its level");
    const OverloadCost<SoftFoldTasks> overload;
    check(levelForHard(overload, false, "'overload_cost'") == Level::Soft &&
                  levelForHard(overload, true, "'overload_cost'") == Level::Hard,
          "a flippable medium constraint: hard=False -> soft, hard=True -> hard");
    OverloadCost<SoftFoldTasks> fixedMedium;
    fixedMedium.levelFlippable = false;
    check(refusal([&] { levelForHard(fixedMedium, false, "'overload_cost'"); }) ==
                          "constraint 'overload_cost' is always medium; hard=False is not allowed" &&
                  refusal([&] { levelForHard(fixedMedium, true, "'overload_cost'"); }) ==
                          "constraint 'overload_cost' is always medium; hard=True is not allowed",
          "a non-flippable medium constraint refuses both hard=True and hard=False");
    std::printf("\n");
}

// --- (k) LIST kind with a medium level -------------------------------------

int manhattan(const std::pair<int, int>& a, const std::pair<int, int>& b) {
    return std::abs(a.first - b.first) + std::abs(a.second - b.second);
}

// Hard: demand beyond the route's capacity.
template <typename S>
struct RouteDemandCost final : optimaru::UnitConstraint<S> {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr Level kDefaultLevel = Level::Hard;
    explicit RouteDemandCost(double weight = kDefaultWeight)
        : optimaru::UnitConstraint<S>("routeDemandCost", weight, kDefaultLevel) {}
    double cost(const S& s, int, const std::vector<int>& members) const override {
        int load = 0;
        for (int c : members) load += s.demand[c];
        return load > s.capacity ? static_cast<double>(load - s.capacity) : 0.0;
    }
};

// Medium unit: cities beyond seven on a route (its cells live in cacheSoft_).
template <typename S>
struct RouteOverflowCost final : optimaru::UnitConstraint<S> {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr Level kDefaultLevel = Level::Medium;
    explicit RouteOverflowCost(double weight = kDefaultWeight)
        : optimaru::UnitConstraint<S>("routeOverflowCost", weight, kDefaultLevel) {}
    double cost(const S&, int, const std::vector<int>& members) const override {
        return members.size() > 7 ? static_cast<double>(members.size() - 7) : 0.0;
    }
};

// Medium global with the preview contract: the longest route, in cities
// (previews fold it through accumulate()).
template <typename S>
struct LongestRouteCost final : optimaru::GlobalConstraint<S> {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr Level kDefaultLevel = Level::Medium;
    explicit LongestRouteCost(double weight = kDefaultWeight)
        : optimaru::GlobalConstraint<S>("longestRouteCost", weight, kDefaultLevel) {}
    std::vector<int> sizes;
    void reset(const S&, const std::vector<std::vector<int>>& membersPerUnit) override {
        sizes.clear();
        for (const auto& m : membersPerUnit) sizes.push_back(static_cast<int>(m.size()));
    }
    void onUnitChanged(const S&, int unit, const std::vector<int>& members) override {
        sizes[unit] = static_cast<int>(members.size());
    }
    double cost() const override {
        return static_cast<double>(*std::max_element(sizes.begin(), sizes.end()));
    }
    bool supportsPreview() const override { return true; }
    double previewCost(const S&, const optimaru::PreviewedUnit* changes, int count) const override {
        int longest = 0;
        for (std::size_t u = 0; u < sizes.size(); ++u) {
            int size = sizes[u];
            for (int i = 0; i < count; ++i)
                if (changes[i].unit == static_cast<int>(u))
                    size = static_cast<int>(changes[i].members->size());
            longest = std::max(longest, size);
        }
        return static_cast<double>(longest);
    }
};

// Soft: the open path depot -> first -> ... -> last on the grid (order
// sensitive, so a reversal changes it).
template <typename S>
struct RouteDistanceCost final : optimaru::UnitConstraint<S> {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr Level kDefaultLevel = Level::Soft;
    explicit RouteDistanceCost(double weight = kDefaultWeight)
        : optimaru::UnitConstraint<S>("routeDistanceCost", weight, kDefaultLevel) {}
    double cost(const S& s, int, const std::vector<int>& members) const override {
        std::pair<int, int> at{0, 0};
        int total = 0;
        for (int c : members) {
            total += manhattan(at, s.city[c]);
            at = s.city[c];
        }
        return static_cast<double>(total);
    }
};

template <typename S>
optimaru::ConstraintAssembly<S> routeDefaults() {
    optimaru::ConstraintAssembly<S> assembly;
    assembly.template emplace<RouteDemandCost<S>>();
    assembly.template emplace<RouteOverflowCost<S>>();
    assembly.template emplace<LongestRouteCost<S>>();
    assembly.template emplace<RouteDistanceCost<S>>();
    return assembly;
}

// Route 0 holds cities 0-9, route 1 cities 10-17, route 2 cities 18-23:
// overflow 3 / 1 / 0, longest route 10.
template <typename S>
S routesStart() {
    S s;
    for (int c = 0; c < 24; ++c) {
        s.city.emplace_back((c * 7) % 11, (c * 5) % 13);
        s.demand.push_back(1 + c % 4);
    }
    s.routes.assign(3, {});
    for (int c = 0; c < 24; ++c) s.routes[c < 10 ? 0 : (c < 18 ? 1 : 2)].push_back(c);
    return s;
}

template <typename S>
void listMediumPreviews(const std::string& label, double mediumWeight, std::uint64_t seed) {
    S s = routesStart<S>();
    optimaru::DecomposedScoreCalculator<S> calc;
    routeDefaults<S>()
            .weight("routeOverflowCost", mediumWeight)
            .weight("longestRouteCost", mediumWeight)
            .into(calc);
    optimaru::ScoreDirector<S> director(s, calc, optimaru::EnvironmentMode::FULL_ASSERT);
    director.calculateScore();
    // By hand: medium cells 3w, 1w, 0 and the global 10w. BelowHard rounds
    // each cell and the global like hard, so at w = 0.6 that is
    // llround(1.8) + llround(0.6) + llround(6.0) = 9, not llround(14 x 0.6) = 8.
    const double w = mediumWeight;
    const double expected =
            optimaru::MediumFoldOf<S>::value.roundsMedium()
                    ? static_cast<double>(std::llround(3 * w) + std::llround(w) + std::llround(10 * w))
                    : 14.0 * w;
    const double medium = calc.levelScores().medium;  // a score: -penalty
    check(std::fabs(medium + expected) < 1e-9,
          label + ": the medium penalty at the start is " + std::to_string(expected) + ", got " +
                  std::to_string(-medium));
    optimaru::testing::PreviewParity parity;
    bool corrupted = false;
    try {
        parity = optimaru::testing::runPreviewParity(director, calc, seed, 4000);
    } catch (const optimaru::ScoreCorruptionError& error) {
        corrupted = true;
        std::printf("    %s\n", error.what());
    }
    std::printf("    %d previews checked, %d declined\n", parity.checked, parity.declined);
    check(!corrupted && parity.allEqual() && parity.declined == 0 && parity.checked > 1000,
          label + ": every preview equals the applied score (FULL_ASSERT: == a full recompute)");
}

void testListMediumPreviews() {
    std::printf("== (k) LIST kind with a medium level: previews ==\n");
    listMediumPreviews<SoftFoldRoutes>("IntoSoft, medium weight 1", 1.0, 41);
    listMediumPreviews<SoftFoldRoutes>("IntoSoft, medium weight 0.37", 0.37, 43);
    listMediumPreviews<HardFoldRoutes>("BelowHard, medium weight 1", 1.0, 47);
    listMediumPreviews<HardFoldRoutes>("BelowHard, medium weight 0.6", 0.6, 53);
    std::printf("\n");
}

// (l) A hard unit constraint charging a big constant per busy worker: with
// a weight near 2^62, two or three busy workers already sum past int64.
template <typename S>
struct BusyWorkerCost final : optimaru::UnitConstraint<S> {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr Level kDefaultLevel = Level::Hard;
    explicit BusyWorkerCost(double weight = kDefaultWeight)
        : optimaru::UnitConstraint<S>("busyWorkerCost", weight, kDefaultLevel) {}
    double cost(const S&, int, const std::vector<int>& members) const override {
        return members.empty() ? 0.0 : 1.0;
    }
};

template <typename S>
HardSoftScore busyScore(double weight, std::vector<int> assignment) {
    S s;
    s.assignment = std::move(assignment);
    optimaru::ConstraintAssembly<S> assembly;
    assembly.template emplace<BusyWorkerCost<S>>(weight);
    return scoreOf(std::move(assembly), s);
}

bool throwsOverflow(const std::function<void()>& run) {
    try {
        run();
    } catch (const std::overflow_error&) {
        return true;
    }
    return false;
}

// Incremental == fresh recompute on a random walk whose busy-worker count
// crosses the saturation boundary both ways.
template <typename S>
bool saturatingWalk(double weight, unsigned seed) {
    S s;
    s.assignment = {0, 0, 0, 0, 0, 0};
    optimaru::DecomposedScoreCalculator<S> calc;
    {
        optimaru::ConstraintAssembly<S> assembly;
        assembly.template emplace<BusyWorkerCost<S>>(weight);
        assembly.template emplace<ConflictCost<S>>();
        assembly.into(calc);
    }
    calc.resetWorkingSolution(s);
    std::mt19937 rng(seed);
    bool agree = true;
    for (int step = 0; step < 400; ++step) {
        const int task = static_cast<int>(rng() % 6);
        const int worker = static_cast<int>(rng() % 4);
        calc.beforeVariableChanged(task);
        s.assignment[task] = worker;
        calc.afterVariableChanged(task);
        optimaru::DecomposedScoreCalculator<S> fresh;
        optimaru::ConstraintAssembly<S> assembly;
        assembly.template emplace<BusyWorkerCost<S>>(weight);
        assembly.template emplace<ConflictCost<S>>();
        assembly.into(fresh);
        fresh.resetWorkingSolution(s);
        agree &= calc.calculateScore() == fresh.calculateScore();
    }
    return agree;
}

void testScoreRange() {
    std::printf("== (l) The range of a score ==\n");
    constexpr std::int64_t kLimit = std::numeric_limits<std::int64_t>::max();
    // Fold-less: 4e18 per busy worker. One and two busy workers fit int64.
    check(busyScore<Tasks>(4e18, {0, 0, 0, 0, 0, 0}).hard == -4000000000000000000LL,
          "one busy worker at 4e18: exact");
    check(busyScore<Tasks>(4e18, {0, 0, 0, 1, 1, 1}).hard == -8000000000000000000LL,
          "two busy workers at 4e18: exact");
    const HardSoftScore three = busyScore<Tasks>(4e18, {0, 0, 1, 1, 2, 2});
    check(three.hard == -kLimit,
          "three busy workers at 4e18 saturate at -INT64_MAX, got " + str(three));
    check(busyScore<Tasks>(4e18, {0, 1, 2, 3, 0, 1}).hard == -kLimit,
          "four busy workers saturate too (worse never looks better)");
    // BelowHard(100): 4e16 hard units per busy worker; the fold multiplies by 100.
    check(busyScore<HardFoldTasks>(4e16, {0, 0, 0, 0, 0, 0}).hard == -4000000000000000000LL,
          "BelowHard: one busy worker at 4e16 folds exactly (x100)");
    check(busyScore<HardFoldTasks>(4e16, {0, 0, 0, 1, 1, 1}).hard == -8000000000000000000LL,
          "BelowHard: two busy workers at 4e16 fold exactly (8e18 fits)");
    const HardSoftScore folded = busyScore<HardFoldTasks>(4e16, {0, 0, 1, 1, 2, 2});
    check(folded.hard == -kLimit,
          "BelowHard: three busy workers at 4e16 fold past int64 -> -INT64_MAX, got " + str(folded));
    check(kHardFold.toEngine(-100000000000000000LL, -5.0, 0.0).hard == -kLimit,
          "MediumFold::toEngine(BelowHard) saturates like the calculator");
    check(kHardFold.toEngine(-3, -5.0, 0.0).hard == -305,
          "MediumFold::toEngine(BelowHard) is exact in range");
    check(saturatingWalk<Tasks>(3e18, 61), "incremental == fresh across saturation (fold-less)");
    check(saturatingWalk<HardFoldTasks>(3e16, 67), "incremental == fresh across saturation (BelowHard)");

    // HardTotal: wraps and comes back exactly.
    optimaru::HardTotal total;
    for (int i = 0; i < 5; ++i) total.add(4000000000000000000LL);
    check(!total.fits() && total.saturated() == kLimit && total.positive(),
          "HardTotal: 5 x 4e18 does not fit and saturates at +INT64_MAX");
    for (int i = 0; i < 5; ++i) total.subtract(4000000000000000000LL);
    check(total.isZero() && total.fits(), "HardTotal: subtracting them again is exact (0)");

    // Rule 1: one contribution out of range throws overflow_error naming it.
    Tasks t = assignmentA<Tasks>();
    check(throwsOverflow([&] {
              optimaru::ConstraintAssembly<Tasks> assembly;
              assembly.emplace<ConflictCost<Tasks>>(1e19);
              scoreOf(std::move(assembly), t);
          }),
          "a hard contribution of 2^62 or more throws overflow_error");
    check(throwsOverflow([&] {
              optimaru::ConstraintAssembly<Tasks> assembly;
              assembly.emplace<TaskCostCost<Tasks>>();
              assembly.exponent("taskCostCost", 400.0);
              scoreOf(std::move(assembly), t);
          }),
          "a non-finite soft contribution (cost^400) throws overflow_error");
    check(throwsOverflow([&] {
              optimaru::ConstraintAssembly<Tasks> assembly;
              assembly.emplace<TaskCostCost<Tasks>>(0.0);
              assembly.exponent("taskCostCost", 400.0);
              scoreOf(std::move(assembly), t);
          }),
          "weight 0 x inf (NaN) throws too, like Python's cost**exponent OverflowError");
    std::string message;
    try {
        optimaru::ConstraintAssembly<Tasks> assembly;
        assembly.emplace<ConflictCost<Tasks>>(1e19);
        scoreOf(std::move(assembly), t);
    } catch (const std::overflow_error& error) {
        message = error.what();
    }
    check(message.find("'conflictCost'") != std::string::npos,
          "the overflow message names the constraint: " + message);
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
    testBindingLevelGuard();
    testListMediumPreviews();
    testScoreRange();
    if (failures == 0) {
        std::printf("All constraint-level tests PASSED.\n");
        return 0;
    }
    std::printf("%d constraint-level test(s) FAILED.\n", failures);
    return 1;
}

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
//   (j) python::checkedLevel(): the bindings' level guard and its
//       wording (applyKnobs / applyConstraints raise it as ValueError)
//   (k) LIST kind with a medium level: every preview (unit cells, a
//       previewing global, BelowHard rounding) equals the applied score,
//       which FULL_ASSERT checks against a full recompute
//   (l) the range of a score: a hard total beyond int64 saturates at
//       -INT64_MAX (never wraps to a feasible-looking score), the BelowHard
//       fold saturates too, incremental == fresh across the saturation
//       boundary, and an out-of-range contribution throws overflow_error;
//       a weight-0 constraint contributes exactly 0 whatever its exponent
//   (m) python::restateKnobs(): the one knob rule of both binding paths
//       (applyConstraints, applyKnobs); the exponent is always applied, so
//       an explicit 1.0 resets a component built with another exponent
//   (n) constraintCosts() reports each component's default weight / level
//       next to the re-stated ones (what weight=None / level=None mean)
//   (o) offset / clip: the threshold sign(x) max(0, |x| - offset)^e and the
//       literal (x - offset)^e, by hand, per unit for a unit constraint
//       (zero-raw units and a scoped constraint's units in literal mode,
//       negative offsets, rewards, hard rounding) and on the total for a
//       global; offset 0 is the 0.2.0 shaping bit for bit; the knob checks
//   (p) several instances of one name: addInstance()/addInstances(), by-name
//       edits on the first instance, each instance scored on its own with
//       its own knobs, constraintCosts() in instance order; incremental ==
//       fresh recompute on random walks with repeated and offset picks (both
//       folds, rejected trial moves), and LIST previews under FULL_ASSERT
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    std::printf("== (j) python::checkedLevel: the bindings' level guard ==\n");
    using optimaru::MediumFold;
    using optimaru::python::checkedLevel;
    using optimaru::python::levelNamed;
    const MediumFold none = MediumFold::none();
    const MediumFold intoSoft = MediumFold::intoSoft(1000.0);
    const MediumFold belowHard = MediumFold::belowHard(100);
    check(levelNamed("hard") == Level::Hard && levelNamed("medium") == Level::Medium &&
                  levelNamed("soft") == Level::Soft && !levelNamed("Hard") && !levelNamed(""),
          "a level is named exactly 'hard', 'medium' or 'soft'");
    // What solve(..., constraints=[RewardCost(level="hard")]) and
    // StructuralCost(level="soft") raise as ValueError through applyConstraints /
    // applyKnobs, in the Python base's words.
    const PreferenceReward<SoftFoldTasks> reward;
    check(refusal([&] { checkedLevel(reward, Level::Hard, intoSoft, "'preference_reward'"); }) ==
                  "constraint 'preference_reward' is a reward (raw cost <= 0) and cannot be hard",
          "level='hard' on a reward is refused: 'is a reward'");
    check(checkedLevel(reward, Level::Soft, intoSoft, "'preference_reward'") == Level::Soft,
          "level='soft' on a soft reward re-states its level");
    check(refusal([&] { checkedLevel(reward, Level::Medium, belowHard, "'preference_reward'"); }) ==
                  "constraint 'preference_reward' is a reward (raw cost <= 0) and cannot be "
                  "medium here, where medium counts toward the hard score",
          "level='medium' on a reward is refused where medium lands in the hard level");
    const ForbiddenWorkerCost<SoftFoldTasks> forbidden;
    check(refusal([&] { checkedLevel(forbidden, Level::Soft, intoSoft, "'forbidden_worker_cost'"); }) ==
                  "constraint 'forbidden_worker_cost' is always hard; level='soft' is not allowed",
          "level='soft' on a non-flippable hard constraint is refused: 'is always hard'");
    check(refusal([&] { checkedLevel(forbidden, Level::Medium, intoSoft, "'forbidden_worker_cost'"); }) ==
                  "constraint 'forbidden_worker_cost' is always hard; level='medium' is not allowed",
          "level='medium' on it is refused too");
    check(checkedLevel(forbidden, Level::Hard, intoSoft, "'forbidden_worker_cost'") == Level::Hard,
          "level='hard' on it re-states its level");
    const OverloadCost<SoftFoldTasks> overload;
    check(checkedLevel(overload, Level::Soft, intoSoft, "'overload_cost'") == Level::Soft &&
                  checkedLevel(overload, Level::Hard, intoSoft, "'overload_cost'") == Level::Hard &&
                  checkedLevel(overload, Level::Medium, intoSoft, "'overload_cost'") == Level::Medium,
          "a flippable medium constraint takes every level");
    check(refusal([&] { checkedLevel(overload, Level::Medium, none, "'overload_cost'"); }) ==
                  "constraint 'overload_cost': this use case has no medium level",
          "level='medium' on a domain without a medium fold is refused");
    OverloadCost<SoftFoldTasks> fixedMedium;
    fixedMedium.levelFlippable = false;
    check(refusal([&] { checkedLevel(fixedMedium, Level::Soft, intoSoft, "'overload_cost'"); }) ==
                          "constraint 'overload_cost' is always medium; level='soft' is not allowed" &&
                  refusal([&] { checkedLevel(fixedMedium, Level::Hard, intoSoft, "'overload_cost'"); }) ==
                          "constraint 'overload_cost' is always medium; level='hard' is not allowed",
          "a non-flippable medium constraint refuses level='hard' and level='soft'");
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
    // Weight 0 is exactly 0 whatever the exponent: the shaped value is never
    // computed, so cost^400 = inf cannot turn into 0 x inf = NaN (an opt-in
    // constraint left at weight 0 must not make a selection unscorable).
    bool weightZeroThrew = false;
    HardSoftScore weightZero;
    try {
        optimaru::ConstraintAssembly<Tasks> assembly;
        assembly.emplace<TaskCostCost<Tasks>>(0.0);
        assembly.exponent("taskCostCost", 400.0);
        assembly.emplace<ConflictCost<Tasks>>(0.0);
        assembly.exponent("conflictCost", 2000.0);  // worker 0's 3 conflicts: 3^2000 = inf
        weightZero = scoreOf(std::move(assembly), t);
    } catch (const std::overflow_error&) {
        weightZeroThrew = true;
    }
    check(!weightZeroThrew && weightZero.hard == 0 && weightZero.soft == 0.0 &&
                  !std::signbit(weightZero.soft),
          "weight 0 x cost^400 / cost^2000 contributes exactly 0 (soft and hard), got " +
                  str(weightZero));
    {
        // Every path: reset, incremental moves (applyRaw), the unit totals,
        // a medium global's total in levelScores() / constraintTotals().
        HardFoldTasks h;
        h.assignment = {0, 0, 0, 0, 0, 0};  // 3 idle workers: 3^2000 = inf
        optimaru::ConstraintAssembly<HardFoldTasks> assembly;
        assembly.emplace<IdleWorkerCost<HardFoldTasks>>(0.0);
        assembly.exponent("idleWorkerCost", 2000.0);
        assembly.emplace<OverloadCost<HardFoldTasks>>(0.0);
        assembly.exponent("overloadCost", 2000.0);  // 4 tasks over: 4^2000 = inf
        assembly.emplace<TaskCostCost<HardFoldTasks>>();
        optimaru::DecomposedScoreCalculator<HardFoldTasks> calc;
        assembly.into(calc);
        bool threw = false;
        bool allZero = true;
        double soft = 0.0;
        try {
            calc.resetWorkingSolution(h);
            const auto levels = calc.levelScores();
            allZero = levels.hard == 0 && levels.medium == 0.0;
            for (const auto& total : calc.constraintTotals()) {
                if (total.name != "taskCostCost" && total.cost != 0.0) allZero = false;
            }
            calc.beforeVariableChanged(5);
            h.assignment[5] = 1;
            calc.afterVariableChanged(5);
            const HardSoftScore moved = calc.calculateScore();
            allZero = allZero && moved.hard == 0;
            soft = moved.soft;
        } catch (const std::overflow_error&) {
            threw = true;
        }
        check(!threw && allZero && soft == -(7.0 + 2 + 6 + 5 + 8 + 6),
              "weight 0 at a huge exponent is 0 on reset, after a move, in levelScores() "
              "and in constraintTotals() (only the weighted taskCostCost scores)");
    }
    std::string message;
    std::string named;
    bool beyond = false;
    try {
        optimaru::ConstraintAssembly<Tasks> assembly;
        assembly.emplace<ConflictCost<Tasks>>(1e19);
        scoreOf(std::move(assembly), t);
    } catch (const optimaru::ContributionOverflowError& error) {
        message = error.what();
        named = error.constraint();
        beyond = error.beyondHardLimit();
    }
    check(message.rfind("constraint 'conflictCost': a hard contribution of ", 0) == 0 &&
                  named == "conflictCost" && beyond,
          "the overflow error names the constraint (registry id in C++): " + message);
    // A non-finite hard contribution is reported as not finite, as in the Python
    // mirror (which checks finiteness first); the bindings print it with the
    // Python name and repr(), word for word the mirror's text.
    message.clear();
    beyond = true;
    try {
        optimaru::ConstraintAssembly<Tasks> assembly;
        assembly.emplace<ConflictCost<Tasks>>();
        assembly.exponent("conflictCost", 2000.0);  // worker 0's 3 conflicts: 3^2000 = inf
        scoreOf(std::move(assembly), t);
    } catch (const optimaru::ContributionOverflowError& error) {
        message = error.what();
        beyond = error.beyondHardLimit();
    }
    check(message.find("is not finite (inf)") != std::string::npos && !beyond,
          "a non-finite hard contribution is reported as not finite: " + message);
    check(optimaru::ContributionOverflowError::message("conflict_cost", "3e+19", true) ==
                  "constraint 'conflict_cost': a hard contribution of 3e+19 exceeds the engine's "
                  "limit of 2**62 (about 4.6e18) per unit; lower its weight or exponent",
          "the hard-limit message reads like the Python mirror's");
    std::printf("\n");
}

// --- (m) restateKnobs ------------------------------------------------------

void testRestateKnobs() {
    std::printf("== (m) python::restateKnobs: the knob rule of both binding paths ==\n");
    using optimaru::python::Knobs;
    using optimaru::python::restateKnobs;
    // A default assembly whose component is built with exponent 2 (no shipped
    // domain does, but a domain factory may): every pick re-states the
    // exponent, an explicit 1.0 included, as the Python score() mirror reads
    // a pick (spec.exponent, 1.0 when unset).
    auto shaped = [] {
        optimaru::ConstraintAssembly<Tasks> assembly;
        assembly.emplace<TaskCostCost<Tasks>>();
        assembly.exponent("taskCostCost", 2.0);
        return assembly;
    };
    Tasks t = assignmentA<Tasks>();
    {
        auto assembly = shaped();
        Knobs knobs;
        knobs.exponent = 1.0;
        restateKnobs(assembly, "taskCostCost", knobs, "'task_cost_cost'");
        check(assembly.find("taskCostCost")->exponent == 1.0,
              "an explicit exponent 1.0 resets a component built with exponent 2");
        check(scoreOf(std::move(assembly), t).soft == -28.0,
              "...and the assembly scores unshaped: soft -28 (not -(15^2 + 11^2 + 2^2))");
    }
    {
        auto assembly = shaped();
        restateKnobs(assembly, "taskCostCost", Knobs{}, "'task_cost_cost'");
        check(assembly.find("taskCostCost")->exponent == 1.0,
              "an unset exponent (None) reads as 1.0 and resets it too");
    }
    {
        auto assembly = shaped();
        Knobs knobs;
        knobs.exponent = 3.0;
        restateKnobs(assembly, "taskCostCost", knobs, "'task_cost_cost'");
        check(assembly.find("taskCostCost")->exponent == 3.0, "exponent 3 is applied");
    }
    {
        auto assembly = defaults<SoftFoldTasks>();
        Knobs knobs;
        knobs.weight = 5.0;
        knobs.level = Level::Soft;
        restateKnobs(assembly, "conflictCost", knobs, "'conflict_cost'");
        const optimaru::ConstraintMetadata* conflict = assembly.find("conflictCost");
        check(conflict->weight == 5.0 && conflict->level == Level::Soft,
              "weight 5, level soft: re-stated on the assembly");
        Knobs keep;  // weight / level unset keep the component's defaults
        restateKnobs(assembly, "overloadCost", keep, "'overload_cost'");
        const optimaru::ConstraintMetadata* overload = assembly.find("overloadCost");
        check(overload->weight == 1.0 && overload->level == Level::Medium,
              "weight / level unset keep the default weight and the medium level");
        Knobs hard;
        hard.level = Level::Hard;
        check(refusal([&] {
                  restateKnobs(assembly, "preferenceReward", hard, "'preference_reward'");
              }) == "constraint 'preference_reward' is a reward (raw cost <= 0) and cannot be hard",
              "level='hard' on a reward is refused in the Python base's words");
        Knobs medium;
        medium.level = Level::Medium;
        restateKnobs(assembly, "taskCostCost", medium, "'task_cost_cost'");
        check(assembly.find("taskCostCost")->level == Level::Medium,
              "level='medium' moves a soft constraint onto the medium level");
    }
    {
        // applyKnobs' path: a component built outside any assembly.
        TaskCostCost<Tasks> bare;
        bare.setExponent(2.0);
        Knobs knobs;
        knobs.exponent = 1.0;
        restateKnobs(bare, knobs, optimaru::MediumFold::none(), "'task_cost_cost'");
        check(bare.exponent == 1.0,
              "the component path applies an explicit 1.0 the same way as the assembly path");
        Knobs soft;
        soft.level = Level::Soft;
        soft.weight = 2.5;
        ForbiddenWorkerCost<Tasks> forbidden;
        check(refusal([&] { restateKnobs(forbidden, soft, kSoftFold, "'forbidden_worker_cost'"); }) ==
                      "constraint 'forbidden_worker_cost' is always hard; level='soft' is not allowed",
              "level='soft' on a non-flippable component is refused on the component path too");
    }
    std::printf("\n");
}

// --- (n) default weight / level in constraintCosts() --------------------------

void testReportedDefaults() {
    std::printf("== (n) constraintCosts(): default weight / level next to the re-stated ones ==\n");
    SoftFoldTasks s = assignmentA<SoftFoldTasks>();
    auto assembly = defaults<SoftFoldTasks>();
    assembly.weight("conflictCost", 5.0).soft("conflictCost");
    assembly.hard("overloadCost");
    assembly.weight("preferenceReward", 2.0);
    optimaru::DecomposedScoreCalculator<SoftFoldTasks> calc;
    assembly.into(calc);
    calc.resetWorkingSolution(s);
    calc.calculateScore();
    bool conflict = false;
    bool overload = false;
    bool reward = false;
    bool untouched = true;
    for (const auto& entry : calc.constraintCosts()) {
        if (entry.name == "conflictCost") {
            conflict = entry.weight == 5.0 && entry.level == Level::Soft &&
                       entry.defaultWeight == 1.0 && entry.defaultLevel == Level::Hard;
        } else if (entry.name == "overloadCost") {
            overload = entry.level == Level::Hard && entry.defaultLevel == Level::Medium;
        } else if (entry.name == "preferenceReward") {
            reward = entry.weight == 2.0 && entry.defaultWeight == 0.5;
        } else if (entry.defaultWeight != entry.weight || entry.defaultLevel != entry.level) {
            untouched = false;
        }
    }
    check(conflict, "conflictCost re-stated to weight 5 / soft reports default 1 / hard");
    check(overload, "overloadCost made hard reports its default level medium");
    check(reward, "preferenceReward at weight 2 reports its default weight 0.5");
    check(untouched, "a component left alone reports default == current");
    // A wrapper copying its inner component's metadata takes the defaults too.
    TaskCostCost<Tasks> inner(4.0);
    inner.setWeight(9.0);
    TaskCostCost<Tasks> copy;
    static_cast<optimaru::ConstraintMetadata&>(copy) = inner;
    check(copy.weight == 9.0 && copy.defaultWeight == 4.0,
          "a metadata copy carries the inner component's construction weight as its default");
    std::printf("\n");
}


// --- (o) offset / clip ----------------------------------------------------

// A scoped unit constraint: one raw unit per task on workers 0 and 2 only
// (scopedUnits() = {2, 0}); the other cells are never evaluated.
template <typename S>
struct ScopedLoadCost final : optimaru::UnitConstraint<S> {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr Level kDefaultLevel = Level::Soft;
    explicit ScopedLoadCost(double weight = kDefaultWeight)
        : optimaru::UnitConstraint<S>("scopedLoadCost", weight, kDefaultLevel) {}
    std::vector<int> scope{2, 0};
    const std::vector<int>* scopedUnits() const override { return &scope; }
    double cost(const S&, int unit, const std::vector<int>& members) const override {
        return unit == 0 || unit == 2 ? static_cast<double>(members.size()) : 0.0;
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const S&, int unit, const std::vector<int>&, int) const override {
        return unit == 0 || unit == 2 ? 1.0 : 0.0;
    }
};

template <typename S>
optimaru::ConstraintAssembly<S> one(const std::string& name) {
    optimaru::ConstraintAssembly<S> assembly = defaults<S>();
    assembly.only({name});
    return assembly;
}

double softOf(optimaru::ConstraintAssembly<Tasks> assembly, Tasks& t) {
    return -scoreOf(std::move(assembly), t).soft;  // the soft penalty
}

void testOffsetClip() {
    std::printf("== (o) offset / clip ==\n");
    using optimaru::shapeCost;
    // The formula itself.
    check(shapeCost(15.0, 1.0, 5.0, true) == 10.0 && shapeCost(3.0, 1.0, 5.0, true) == 0.0 &&
                  shapeCost(0.0, 2.0, 5.0, true) == 0.0 && shapeCost(-8.0, 2.0, 5.0, true) == -9.0,
          "threshold: sign(x) * max(0, |x| - offset)^e (15 -> 10, 3 -> 0, 0 -> 0, -8 -> -9 at e 2)");
    check(shapeCost(15.0, 3.0, 10.0, false) == 125.0 && shapeCost(7.0, 3.0, 10.0, false) == -27.0 &&
                  shapeCost(0.0, 1.0, 10.0, false) == -10.0,
          "literal: shape(x - offset, e) ((15-10)^3 = 125, (7-10)^3 = -27, a raw 0 -> -10)");
    check(shapeCost(4.0, 2.0, -1.0, true) == 25.0 && shapeCost(0.0, 2.0, -1.0, true) == 0.0 &&
                  shapeCost(-4.0, 1.0, -1.0, true) == -5.0 && shapeCost(0.0, 2.0, -1.0, false) == 1.0,
          "a negative offset: threshold adds |offset| to every non-zero x, literal to every x");
    bool same020 = true;
    for (double x : {-7.5, -1.0, -0.0, 0.0, 0.3, 1.0, 2.0, 17.25, 1e10}) {
        for (double e : {0.5, 1.0, 2.0, 3.0}) {
            for (bool clip : {true, false}) {
                const double a = shapeCost(x, e, 0.0, clip);
                const double b = shapeCost(x, e);
                same020 &= std::memcmp(&a, &b, sizeof a) == 0;
            }
        }
    }
    check(same020, "offset 0 is the 0.2.0 shapeCost bit for bit, in both modes");
    check(optimaru::weightedCost(0.0, 0.0, 400.0, 10.0, false) == 0.0 &&
                  optimaru::weightedCost(0.0, 1e300, 50.0, -3.0, true) == 0.0,
          "weight 0 contributes exactly 0 at any offset / exponent");

    // Unit constraint, assignment A: taskCostCost per worker [15, 11, 0, 2].
    Tasks t = assignmentA<Tasks>();
    check(softOf(one<Tasks>("taskCostCost").offset("taskCostCost", 5.0), t) == 16.0,
          "threshold offset 5: [10, 6, 0, 0] = 16 soft");
    check(softOf(one<Tasks>("taskCostCost").offset("taskCostCost", 5.0).clip("taskCostCost", false),
                 t) == 8.0,
          "literal offset 5: [10, 6, -5, -3] = 8 soft (the empty worker 2 is rewarded too)");
    check(softOf(one<Tasks>("taskCostCost").offset("taskCostCost", 5.0).exponent("taskCostCost", 2.0),
                 t) == 136.0,
          "threshold offset 5, exponent 2: 100 + 36 = 136");
    check(softOf(one<Tasks>("taskCostCost")
                         .offset("taskCostCost", 5.0)
                         .exponent("taskCostCost", 2.0)
                         .clip("taskCostCost", false),
                 t) == 102.0,
          "literal offset 5, exponent 2: 100 + 36 - 25 - 9 = 102 (sign-preserving)");
    check(softOf(one<Tasks>("taskCostCost").offset("taskCostCost", -1.0), t) == 31.0 &&
                  softOf(one<Tasks>("taskCostCost").offset("taskCostCost", -1.0).clip("taskCostCost",
                                                                                      false),
                         t) == 32.0,
          "offset -1: threshold [16, 12, 0, 3] = 31, literal [16, 12, 1, 3] = 32");
    // A reward (raw [-3, 0, 0, 0], weight 0.5) next to the 28 of taskCostCost.
    auto rewardPair = [] {
        auto assembly = defaults<Tasks>();
        assembly.only({"taskCostCost", "preferenceReward"});
        return assembly;
    };
    check(softOf(rewardPair().offset("preferenceReward", 1.0), t) == 27.0,
          "threshold on a reward: -max(0, 3 - 1) x 0.5 = -1, soft 28 - 1 = 27");
    check(softOf(rewardPair().offset("preferenceReward", 1.0).clip("preferenceReward", false), t) ==
                  24.5,
          "literal on a reward: [-4, -1, -1, -1] x 0.5 = -3.5, soft 28 - 3.5 = 24.5");
    // Hard rounding per unit: conflictCost [2, 1, 0, 0], literal offset 0.5 ->
    // [1.5, 0.5, -0.5, -0.5], llround 2 + 1 - 1 - 1 = 1 (not llround(1.0)).
    check(scoreOf(one<Tasks>("conflictCost").offset("conflictCost", 0.5).clip("conflictCost", false),
                  t)
                          .hard == -1,
          "literal offset 0.5 on a hard constraint: rounded per unit, half away from zero");
    check(scoreOf(one<Tasks>("conflictCost").offset("conflictCost", 1.0).clip("conflictCost", false),
                  t)
                          .hard == 1,
          "literal offset 1 on a hard constraint: [1, 0, -1, -1] -> +1 hard (a reward below 1)");
    // Global constraint: shaped on its single total.
    Tasks c;
    c.assignment = {0, 0, 0, 1, 1, 1};  // idle workers 2 and 3: total 2
    auto idle = [] {
        auto assembly = one<Tasks>("idleWorkerCost");
        assembly.soft("idleWorkerCost");
        return assembly;
    };
    check(softOf(idle(), c) == 2.0, "the global's total: 2 idle workers");
    check(softOf(idle().offset("idleWorkerCost", 1.0).exponent("idleWorkerCost", 2.0), c) == 1.0,
          "global threshold offset 1, exponent 2: max(0, 2 - 1)^2 = 1 (on the total, not per worker)");
    check(softOf(idle().offset("idleWorkerCost", 3.0).exponent("idleWorkerCost", 2.0).clip(
                         "idleWorkerCost", false),
                 c) == -1.0,
          "global literal offset 3, exponent 2: shape(2 - 3, 2) = -1");
    check(softOf(idle().offset("idleWorkerCost", 3.0), c) == 0.0,
          "global threshold offset 3: below the offset contributes 0");
    // Scoped unit constraint: literal mode charges only the evaluated units.
    {
        optimaru::ConstraintAssembly<Tasks> scoped;
        scoped.emplace<ScopedLoadCost<Tasks>>();
        scoped.offset("scopedLoadCost", 1.0).clip("scopedLoadCost", false);
        optimaru::DecomposedScoreCalculator<Tasks> calc;
        scoped.into(calc);
        calc.resetWorkingSolution(t);
        // Worker 0 holds 3 tasks (3 - 1 = 2), worker 2 none (0 - 1 = -1);
        // workers 1 and 3 are not evaluated.
        check(-calc.calculateScore().soft == 1.0,
              "literal offset 1 on a scoped constraint: only workers 0 and 2 count (2 - 1 = 1)");
        const auto entries = calc.constraintCosts();
        check(entries.size() == 1 && entries[0].scoped &&
                      entries[0].units == std::vector<int>{0, 2} &&
                      entries[0].unitCosts == std::vector<double>{3.0, 0.0, 0.0, 0.0} &&
                      entries[0].offset == 1.0 && !entries[0].clip,
              "constraintCosts(): a scoped constraint reports its evaluated units, sorted, and "
              "the offset / clip");
    }
    {
        optimaru::DecomposedScoreCalculator<Tasks> calc;
        defaults<Tasks>().soft("overloadCost").soft("idleWorkerCost").into(calc);
        calc.resetWorkingSolution(t);
        calc.calculateScore();
        bool plain = true;
        for (const auto& entry : calc.constraintCosts()) {
            plain &= !entry.scoped && entry.units.empty() && entry.offset == 0.0 && entry.clip;
        }
        check(plain, "an unscoped constraint reports no units; the default offset 0, clip true");
    }
    // The checks.
    check(throwsInvalid([] {
              defaults<Tasks>().offset("taskCostCost", std::numeric_limits<double>::infinity());
          }) && throwsInvalid([] {
              defaults<Tasks>().offset("taskCostCost", std::numeric_limits<double>::quiet_NaN());
          }),
          "a non-finite offset throws");
    check(!throwsInvalid([] { defaults<Tasks>().offset("taskCostCost", -12.5); }),
          "a negative offset is allowed");
    {
        using optimaru::python::Knobs;
        auto assembly = defaults<Tasks>();
        assembly.offset("taskCostCost", 4.0).clip("taskCostCost", false);
        optimaru::python::restateKnobs(assembly, "taskCostCost", Knobs{}, "'task_cost_cost'");
        const optimaru::ConstraintMetadata* reset = assembly.find("taskCostCost");
        check(reset->offset == 0.0 && reset->clip,
              "restateKnobs: an unset offset / clip reads as 0 / true and resets the component");
        Knobs knobs;
        knobs.offset = -2.0;
        knobs.clip = false;
        TaskCostCost<Tasks> bare;
        optimaru::python::restateKnobs(bare, knobs, optimaru::MediumFold::none(),
                                       "'task_cost_cost'");
        check(bare.offset == -2.0 && !bare.clip, "restateKnobs applies offset / clip to a component");
    }
    {
        // Every 0.2.0 assembly (offset 0) scores exactly as before.
        Tasks a = assignmentA<Tasks>();
        auto plain = [] { return defaults<Tasks>().soft("overloadCost").soft("idleWorkerCost"); };
        const HardSoftScore before = scoreOf(plain(), a);
        const HardSoftScore restated =
                scoreOf(plain().offset("taskCostCost", 0.0).clip("taskCostCost", false), a);
        check(before == restated && before.hard == -4 && before.soft == -28.5,
              "offset 0 with clip false scores exactly like the 0.2.0 shaping (-4hard/-28.5soft)");
    }
    std::printf("\n");
}

// --- (p) several instances of one name ------------------------------------

template <typename S>
optimaru::ConstraintAssembly<S> repeated() {
    // taskCostCost three times: as is, exponent 2 with the threshold at 5, and
    // the literal formula at 10 at weight 0.5; conflictCost twice (the copy
    // with the literal offset 1); idleWorkerCost twice (the copy shaped on
    // its total); overloadCost (full re-cost) twice, the copy shifted.
    auto assembly = defaults<S>();
    assembly.addInstances(
            one<S>("taskCostCost").exponent("taskCostCost", 2.0).offset("taskCostCost", 5.0));
    assembly.addInstances(one<S>("taskCostCost")
                                  .weight("taskCostCost", 0.5)
                                  .offset("taskCostCost", 10.0)
                                  .clip("taskCostCost", false));
    assembly.addInstances(
            one<S>("conflictCost").offset("conflictCost", 1.0).clip("conflictCost", false));
    assembly.addInstances(one<S>("idleWorkerCost")
                                  .offset("idleWorkerCost", 1.0)
                                  .exponent("idleWorkerCost", 2.0)
                                  .clip("idleWorkerCost", false));
    assembly.addInstances(one<S>("overloadCost").offset("overloadCost", -1.0).weight("overloadCost", 2.0));
    return assembly;
}

template <typename S, typename Make>
bool walkAgrees(Make make, unsigned seed) {
    S s = assignmentA<S>();
    optimaru::DecomposedScoreCalculator<S> calc;
    make().into(calc);
    calc.resetWorkingSolution(s);
    std::mt19937_64 rng(seed);
    bool agree = true;
    auto fresh = [&] {
        optimaru::DecomposedScoreCalculator<S> other;
        make().into(other);
        other.resetWorkingSolution(s);
        return other.calculateScore();
    };
    for (int step = 0; step < 2000; ++step) {
        const bool trial = step % 3 == 0;
        const int task = static_cast<int>(rng() % 6);
        const int old = s.assignment[task];
        const int worker = (rng() % 7 == 0) ? -1 : static_cast<int>(rng() % 4);
        if (trial) calc.beginMoveEvaluation();
        calc.beforeVariableChanged(task);
        s.assignment[task] = worker;
        calc.afterVariableChanged(task);
        agree &= same(calc.calculateScore(), fresh());
        if (trial) {
            calc.beginMoveRollback();
            calc.beforeVariableChanged(task);
            s.assignment[task] = old;
            calc.afterVariableChanged(task);
            calc.endMoveEvaluation(false);
            agree &= same(calc.calculateScore(), fresh());
        }
    }
    return agree;
}

// A hard component with a compiled delta group (the employee-scheduling /
// school pattern): its instances share one group, which writes integral raw
// deltas, so an unshaped instance takes the direct grouped-hard fold and a
// shifted one must not (weight * raw delta is not the shaped difference).
template <typename S>
struct GroupedTaskCost;

template <typename S>
struct TaskDeltaGroup final : optimaru::UnitConstraintDeltaGroup<S> {
    std::vector<std::size_t> owned;
    void add(std::size_t index, const optimaru::UnitConstraint<S>&) override {
        owned.push_back(index);
    }
    const std::vector<std::size_t>& indexes() const override { return owned; }
    bool hasIntegralRawDeltas() const override { return true; }
    void onReset(const S&) override {}
    void evaluate(const S& s, int unit, const std::vector<int>&, int entity,
                  double* deltas) override {
        for (std::size_t index : owned) deltas[index] = static_cast<double>(s.cost[entity][unit]);
    }
};

template <typename S>
struct GroupedTaskCost final : optimaru::UnitConstraint<S> {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr Level kDefaultLevel = Level::Hard;
    explicit GroupedTaskCost(double weight = kDefaultWeight)
        : optimaru::UnitConstraint<S>("groupedTaskCost", weight, kDefaultLevel) {}
    double cost(const S& s, int unit, const std::vector<int>& members) const override {
        double c = 0;
        for (int t : members) c += static_cast<double>(s.cost[t][unit]);
        return c;
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const S& s, int unit, const std::vector<int>&, int entity) const override {
        return static_cast<double>(s.cost[entity][unit]);
    }
    const char* deltaGroupKey() const override { return "test.grouped-task"; }
    std::unique_ptr<optimaru::UnitConstraintDeltaGroup<S>> createDeltaGroup() const override {
        return std::make_unique<TaskDeltaGroup<S>>();
    }
};

template <typename S>
optimaru::ConstraintAssembly<S> groupedRepeats() {
    optimaru::ConstraintAssembly<S> assembly;
    assembly.template emplace<GroupedTaskCost<S>>();
    assembly.template emplaceInstance<GroupedTaskCost<S>>();
    assembly.template emplaceInstance<GroupedTaskCost<S>>(2.0);
    auto shifted = std::make_unique<GroupedTaskCost<S>>();
    shifted->setOffset(9.0);
    shifted->setClip(false);
    assembly.addInstance(std::move(shifted));
    auto threshold = std::make_unique<GroupedTaskCost<S>>(3.0);
    threshold->setOffset(10.0);
    assembly.addInstance(std::move(threshold));
    return assembly;
}

template <typename S>
optimaru::ConstraintAssembly<S> routeRepeats() {
    auto assembly = routeDefaults<S>();
    assembly.offset("routeDistanceCost", 20.0).clip("routeDistanceCost", false);
    assembly.offset("longestRouteCost", 8.0);
    auto copy = routeDefaults<S>();
    copy.only({"routeDistanceCost", "longestRouteCost", "routeDemandCost"});
    copy.exponent("routeDistanceCost", 2.0).offset("routeDistanceCost", -3.0);
    copy.offset("longestRouteCost", 9.0).clip("longestRouteCost", false).weight("longestRouteCost", 0.6);
    copy.offset("routeDemandCost", 2.0).clip("routeDemandCost", false).weight("routeDemandCost", 0.5);
    assembly.addInstances(std::move(copy));
    return assembly;
}

template <typename S>
void listRepeatPreviews(const std::string& label, std::uint64_t seed) {
    S s = routesStart<S>();
    optimaru::DecomposedScoreCalculator<S> calc;
    routeRepeats<S>().into(calc);
    optimaru::ScoreDirector<S> director(s, calc, optimaru::EnvironmentMode::FULL_ASSERT);
    director.calculateScore();
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
          label + ": repeated, offset picks: every preview equals the applied score (FULL_ASSERT)");
}

void testInstances() {
    std::printf("== (p) Several instances of one name ==\n");
    Tasks t = assignmentA<Tasks>();
    // By hand on A: taskCostCost [15, 11, 0, 2] = 28; the copy with exponent
    // 2 and the threshold at 5: 10^2 + 6^2 = 136; together 164.
    {
        auto assembly = one<Tasks>("taskCostCost");
        assembly.emplaceInstance<TaskCostCost<Tasks>>();
        assembly.exponent("taskCostCost", 2.0);  // the FIRST instance only
        check(assembly.size() == 2 && assembly.names() ==
                                              std::vector<std::string>{"taskCostCost", "taskCostCost"},
              "emplaceInstance keeps the instance already held");
        check(softOf(std::move(assembly), t) == 15 * 15 + 11 * 11 + 2 * 2 + 28.0,
              "a by-name edit addresses the first instance: 354 (shaped) + 28 (the copy)");
    }
    {
        auto assembly = one<Tasks>("taskCostCost");
        assembly.addInstances(
                one<Tasks>("taskCostCost").exponent("taskCostCost", 2.0).offset("taskCostCost", 5.0));
        check(softOf(std::move(assembly), t) == 28.0 + 136.0,
              "x + max(0, x - 5)^2 per unit: 28 + 136 = 164 (two instances, each its own knobs)");
    }
    {
        auto assembly = one<Tasks>("taskCostCost");
        assembly.addInstances(one<Tasks>("taskCostCost"));
        check(softOf(std::move(assembly), t) == 56.0, "identical instances simply add up (2 x 28)");
    }
    {
        auto assembly = one<Tasks>("taskCostCost");
        assembly.emplaceInstance<TaskCostCost<Tasks>>(3.0);
        assembly.drop("taskCostCost");
        check(assembly.size() == 1 && assembly.find("taskCostCost")->weight == 3.0,
              "drop() removes the first instance; the second becomes the first");
        auto both = defaults<Tasks>();
        both.emplaceInstance<TaskCostCost<Tasks>>(3.0);
        both.only({"taskCostCost"});
        check(both.size() == 2, "only() keeps every instance of the names it is given");
    }
    {
        optimaru::DecomposedScoreCalculator<SoftFoldTasks> calc;
        repeated<SoftFoldTasks>().into(calc);
        SoftFoldTasks a = assignmentA<SoftFoldTasks>();
        calc.resetWorkingSolution(a);
        calc.calculateScore();
        std::vector<std::string> names;
        for (const auto& entry : calc.constraintCosts()) names.push_back(entry.name);
        const std::vector<std::string> keys = optimaru::python::instanceKeys(names);
        check(keys == std::vector<std::string>{"conflictCost", "overloadCost", "taskCostCost",
                                               "preferenceReward", "forbiddenWorkerCost",
                                               "taskCostCost#2", "taskCostCost#3", "conflictCost#2",
                                               "overloadCost#2", "idleWorkerCost",
                                               "idleWorkerCost#2"},
              "constraintCosts(): each name's instances in the order they were added -> name, "
              "name#2, ...");
        bool ownKnobs = false;
        for (const auto& entry : calc.constraintCosts()) {
            if (entry.name == "taskCostCost" && entry.offset == 10.0) {
                ownKnobs = !entry.clip && entry.weight == 0.5 &&
                           entry.unitCosts == std::vector<double>{15.0, 11.0, 0.0, 2.0};
            }
        }
        check(ownKnobs, "a copy reports its own knobs and the same raw costs as the first");
        // By hand, on A (IntoSoft 1000): the defaults give -4hard /
        // -(26.5 + 2 x 1000)soft. The copies add: taskCostCost#2 136,
        // taskCostCost#3 0.5 x (5 + 1 - 10 - 8) = -6, conflictCost#2
        // [1, 0, -1, -1] = -1 hard, overloadCost#2 2 x (1 + 1) = 4 medium
        // (threshold at -1: only worker 0 is non-zero), idleWorkerCost#2
        // shape(1 - 1, 2) = 0 medium.
        const HardSoftScore score = calc.calculateScore();
        check(same(score, {-3, -(26.5 + 136.0 - 6.0 + (2 + 4) * 1000.0)}),
              "the repeated assembly by hand: -3hard / -6156.5soft, got " + str(score));
    }
    check(walkAgrees<SoftFoldTasks>([] { return repeated<SoftFoldTasks>(); }, 23),
          "IntoSoft: repeated + offset picks, incremental == fresh over 2000 moves (1/3 rejected)");
    check(walkAgrees<HardFoldTasks>([] { return repeated<HardFoldTasks>(); }, 29),
          "BelowHard: repeated + offset picks, incremental == fresh over 2000 moves (1/3 rejected)");
    check(walkAgrees<Tasks>(
                  [] {
                      optimaru::ConstraintAssembly<Tasks> assembly;
                      assembly.emplace<ScopedLoadCost<Tasks>>();
                      assembly.offset("scopedLoadCost", 2.0).clip("scopedLoadCost", false);
                      assembly.emplaceInstance<ScopedLoadCost<Tasks>>(2.0);
                      assembly.emplaceInstance<ConflictCost<Tasks>>();
                      return assembly;
                  },
                  31),
          "a scoped literal-offset constraint and its copy: incremental == fresh");
    {
        // By hand on A: groupedTaskCost per worker [15, 11, 0, 2] = 28, hard.
        // Instances: 28 + 28 + 2 x 28 + literal at 9 ([6, 2, -9, -7] = -8)
        // + 3 x threshold at 10 ([5, 1, 0, 0] = 6, x3 = 18) = 122.
        optimaru::DecomposedScoreCalculator<Tasks> calc;
        groupedRepeats<Tasks>().into(calc);
        Tasks a = assignmentA<Tasks>();
        calc.resetWorkingSolution(a);
        check(calc.deltaGroupCount() == 1 && calc.calculateScore().hard == -122,
              "five instances of a grouped hard component share one delta group: -122hard");
    }
    check(walkAgrees<Tasks>([] { return groupedRepeats<Tasks>(); }, 37),
          "grouped deltas: the direct hard fold for the unshaped instances, the shaped path for "
          "the offset ones; incremental == fresh");
    listRepeatPreviews<SoftFoldRoutes>("LIST IntoSoft", 59);
    listRepeatPreviews<HardFoldRoutes>("LIST BelowHard", 61);
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
    testRestateKnobs();
    testReportedDefaults();
    testOffsetClip();
    testInstances();
    if (failures == 0) {
        std::printf("All constraint-level tests PASSED.\n");
        return 0;
    }
    std::printf("%d constraint-level test(s) FAILED.\n", failures);
    return 1;
}

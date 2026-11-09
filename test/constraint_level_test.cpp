// Constraint levels, the medium fold and the knob guards,
// engine test/ style: check() + a non-zero exit on any failure.
//   (a) Level metadata, is_hard() and metadata copies
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
//   (h) python_name(): registry name -> Python name; instance_keys(); a name
//       holding '#' is refused by instance_keys() and by the assembly
//   (i) knob setters on a bare component, labels, wrappers that copy their
//       inner component's metadata, catalog_info() for a catalog wider than
//       the defaults
//   (j) python::checked_level(): the bindings' level guard and its
//       wording (apply_knobs / apply_constraints raise it as ValueError)
//   (k) sequence shape with a medium level: every preview (unit cells, a
//       previewing global, BelowHard rounding) equals the applied score,
//       which ScoreChecks::Full checks against a full recompute
//   (l) the range of a score: a hard total beyond int64 saturates at
//       -INT64_MAX (never wraps to a feasible-looking score), the BelowHard
//       fold saturates too, incremental == fresh across the saturation
//       boundary, and an out-of-range contribution throws overflow_error;
//       a weight-0 constraint contributes exactly 0 whatever its exponent
//   (m) python::restate_knobs(): the one knob rule of both binding paths
//       (apply_constraints, apply_knobs); the exponent is always applied, so
//       an explicit 1.0 resets a component built with another exponent
//   (n) constraint_costs() reports each component's default weight / level
//       next to the re-stated ones (what weight=None / level=None mean)
//   (o) offset / clip: the threshold sign(x) max(0, |x| - offset)^e and the
//       literal (x - offset)^e, by hand, per unit for a unit constraint
//       (zero-raw units and a scoped constraint's units in literal mode,
//       negative offsets, rewards, hard rounding) and on the total for a
//       global; offset 0 is the plain shaping bit for bit; the knob checks
//   (p) several instances of one name: add_instance()/add_instances(), by-name
//       edits on the first instance, each instance scored on its own with
//       its own knobs, constraint_costs() in instance order; incremental ==
//       fresh recompute on random walks with repeated and offset picks (both
//       folds, rejected trial moves), and sequence previews under ScoreChecks::Full
//   (q) the literal offset formula (clip false, offset != 0) stays off the
//       levels that count toward the hard score: python::check_literal_offset
//       (both messages, both offset signs, the default level, a structural
//       hard constraint, the allowed cases) on both restate_knobs paths, and
//       DecomposedObjective::validateLevel at reset (the C++ API in
//       any order, direct components, later edits)
//   (r) the threshold with a negative offset (a fixed charge per non-zero
//       unit, discontinuous at 0) gets exact raw costs: with real-valued
//       deltas, incremental == fresh bit for bit and a unit back at raw 0
//       contributes 0, for a unit constraint (hard and soft), a delta-group
//       member and a global, one move at a time, in a batch and through
//       trial moves; only that instance leaves the delta path (an integral
//       group keeps it); sequence previews skip its piecewise answer
//   (s) the sign rule: an instance whose raw cost can be negative on the
//       problem's data (raw_cost_can_be_negative, not a reward) is refused at
//       reset on a level that counts toward the hard score
//       (SignedCostLevelError, both wordings), unit and global, and
//       constraint_costs() reports it (signed_cost)
//   (t) constraint_totals() / constraint_costs() report the knobs sampled at
//       the last reset, the ones score() uses, after a component
//       was edited without a reset
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

#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/python/level_guard.hpp"
#include "optimaru/python/names.hpp"
#include "optimaru/testing/preview_parity.hpp"

using optimaru::Score;
using optimaru::Level;
using optimaru::MediumFold;

namespace {

int failures = 0;

void check(bool ok, const std::string& label) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}

bool same(const Score& a, const Score& b) {
    return a.hard == b.hard && std::fabs(a.soft - b.soft) < 1e-9;
}

std::string str(const Score& s) { return s.str(); }

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
    static constexpr optimaru::Shape shape = optimaru::Shape::Assignment;
    static int entity_count(const S& s) { return static_cast<int>(s.assignment.size()); }
    static int value_count(const S& s) { return s.workerCount; }
    static int value_of(const S& s, int e) { return s.assignment[e]; }
    static void assign(S& s, int e, int v) { s.assignment[e] = v; }
};

// The domain-side declaration a use case writes once, next to its
// Score adapter.
constexpr MediumFold kSoftFold = MediumFold::into_soft(1000.0);
constexpr MediumFold kHardFold = MediumFold::below_hard(100);

// sequence shape (the shape of vehicle-routing / pickup-delivery): 24 cities on
// three routes, each city with a demand. The two types differ only in their
// medium fold, like the Tasks types above.
struct Routes {
    std::vector<std::pair<int, int>> city;  // grid coordinates per element; the depot is (0, 0)
    std::vector<int> demand;                // per element
    int capacity = 18;
    std::vector<std::vector<int>> routes;   // the list decision variable
};
struct SoftFoldRoutes : Routes {};  // medium folds into soft x1000
struct HardFoldRoutes : Routes {};  // medium folds below hard, 100 per hard unit

template <typename S>
struct RoutesDescriptor {
    static constexpr optimaru::Shape shape = optimaru::Shape::Sequence;
    static int list_count(const S& s) { return static_cast<int>(s.routes.size()); }
    static int element_count(const S& s) { return static_cast<int>(s.city.size()); }
    static int list_size(const S& s, int l) { return static_cast<int>(s.routes[l].size()); }
    static int element_at(const S& s, int l, int p) { return s.routes[l][p]; }
    static void insert_element(S& s, int l, int p, int e) {
        s.routes[l].insert(s.routes[l].begin() + p, e);
    }
    static int remove_element(S& s, int l, int p) {
        const int e = s.routes[l][p];
        s.routes[l].erase(s.routes[l].begin() + p);
        return e;
    }
    static void reverse_segment(S& s, int l, int first, int last) {
        std::reverse(s.routes[l].begin() + first, s.routes[l].begin() + last + 1);
    }
};

}  // namespace

template <>
struct optimaru::SolutionTraits<Tasks> : TasksDescriptor<Tasks> {};
template <>
struct optimaru::SolutionTraits<SoftFoldTasks> : TasksDescriptor<SoftFoldTasks> {};
template <>
struct optimaru::SolutionTraits<HardFoldTasks> : TasksDescriptor<HardFoldTasks> {};
template <>
struct optimaru::MediumFoldOf<SoftFoldTasks> {
    static constexpr optimaru::MediumFold value = kSoftFold;
};
template <>
struct optimaru::MediumFoldOf<HardFoldTasks> {
    static constexpr optimaru::MediumFold value = kHardFold;
};
template <>
struct optimaru::SolutionTraits<SoftFoldRoutes> : RoutesDescriptor<SoftFoldRoutes> {};
template <>
struct optimaru::SolutionTraits<HardFoldRoutes> : RoutesDescriptor<HardFoldRoutes> {};
template <>
struct optimaru::MediumFoldOf<SoftFoldRoutes> {
    static constexpr optimaru::MediumFold value = kSoftFold;
};
template <>
struct optimaru::MediumFoldOf<HardFoldRoutes> {
    static constexpr optimaru::MediumFold value = kHardFold;
};

namespace {

// --- components --------------------------------------------------------------

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
    bool supports_delta() const override { return true; }
    double delta_cost(const S& s, int, const std::vector<int>& members, int entity) const override {
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
    void reset(const S&, const std::vector<std::vector<int>>& members_per_unit) override {
        counts.clear();
        for (const auto& m : members_per_unit) counts.push_back(static_cast<int>(m.size()));
    }
    void on_unit_changed(const S&, int unit, const std::vector<int>& members) override {
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
    bool supports_delta() const override { return true; }
    double delta_cost(const S& s, int unit, const std::vector<int>&, int entity) const override {
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
        this->level_flippable = false;
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
        this->level_flippable = false;
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
Score scoreOf(optimaru::ConstraintAssembly<S> assembly, S& s) {
    optimaru::DecomposedObjective<S> calc;
    assembly.into(calc);
    calc.rebuild(s);
    return calc.score();
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
    check(conflict.is_hard(), "isHard() reads level == Hard");
    check(!conflict.reward && conflict.level_flippable, "reward=false, levelFlippable=true by default");
    conflict.set_level(Level::Soft);
    check(conflict.level == Level::Soft && !conflict.is_hard(), "setLevel(Soft) clears isHard()");
    conflict.set_level(Level::Hard);
    check(conflict.is_hard(), "setLevel(Hard) sets isHard()");

    // A copy owns its own metadata.
    ConflictCost<Tasks> copy = conflict;
    copy.set_level(Level::Soft);
    check(conflict.level == Level::Hard && copy.level == Level::Soft,
          "changing a copied constraint's level leaves the original alone");
    copy = conflict;
    check(copy.is_hard(), "copy assignment copies the level");
    check(std::string(optimaru::level_name(Level::Medium)) == "medium",
          "levelName spells the levels for clients");
    std::printf("\n");
}

void testMediumFold() {
    std::printf("== (b) The medium fold ==\n");
    {
        SoftFoldTasks s = assignmentA<SoftFoldTasks>();
        optimaru::DecomposedObjective<SoftFoldTasks> calc;
        defaults<SoftFoldTasks>().into(calc);
        calc.rebuild(s);
        const Score score = calc.score();
        check(same(score, {-4, -(26.5 + 2 * 1000.0)}),
              "IntoSoft(1000): -4hard / -(26.5 + 2 x 1000)soft, got " + str(score));
        const auto levels = calc.level_scores();
        check(levels.hard == -4 && levels.medium == -2.0 && std::fabs(levels.soft + 26.5) < 1e-9,
              "levelScores() reports the three accumulators before the fold");
        check(same(kSoftFold.to_engine(levels.hard, levels.medium, levels.soft), score),
              "the domain adapter's MediumFold::to_engine == the objective's fold");
        bool levelsReported = true;
        for (const auto& total : calc.constraint_totals()) {
            if (total.name == "overloadCost")
                levelsReported &= total.level == Level::Medium && total.cost == 1.0 && !total.is_hard;
            if (total.name == "conflictCost")
                levelsReported &= total.level == Level::Hard && total.is_hard && total.cost == 3.0;
            if (total.name == "idleWorkerCost")
                levelsReported &= total.level == Level::Medium && total.cost == 1.0;
        }
        check(levelsReported, "constraintTotals() report each level, medium in medium units");
    }
    {
        HardFoldTasks s = assignmentA<HardFoldTasks>();
        const Score score = scoreOf(defaults<HardFoldTasks>(), s);
        check(same(score, {-(4 * 100 + 2), -26.5}),
              "BelowHard(100): -(4 x 100 + 2)hard / -26.5soft, got " + str(score));
    }
    {
        Tasks s = assignmentA<Tasks>();
        check(throwsLogic(defaults<Tasks>(), s),
              "a medium constraint on a domain without a medium fold throws at reset");
        const Score score = scoreOf(
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
              reward->level_flippable = true;  // even a flippable reward
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
        ok = rows[i].name == expected[i].name && rows[i].default_level == expected[i].level &&
             rows[i].default_weight == expected[i].weight &&
             rows[i].level_flippable == expected[i].flippable &&
             rows[i].reward == expected[i].reward && rows[i].is_global == expected[i].global;
    }
    check(ok, "one row per component with its default level, weight, flippability, reward");
    const auto names = defaults<SoftFoldTasks>().names();
    bool sameOrder = names.size() == rows.size();
    for (std::size_t i = 0; sameOrder && i < rows.size(); ++i) sameOrder = names[i] == rows[i].name;
    check(sameOrder, "info() rows follow names() order");
    const auto edited = defaults<SoftFoldTasks>().weight("taskCostCost", 4.0).info();
    check(edited[2].default_weight == 4.0,
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
    check(std::fabs(optimaru::shape_cost(-3.0, 2.0) + 9.0) < 1e-12 &&
                  std::fabs(optimaru::shape_cost(-4.0, 0.5) + 2.0) < 1e-12,
          "shapeCost raises the magnitude of a negative raw cost and keeps its sign");
    check(same(scoreOf(defaults<SoftFoldTasks>().exponent("preferenceReward", 2.0), a),
               {-4, -(23.5 + 2 * 1000.0)}),
          "exponent 2 on a reward: raw -3 -> -9, x0.5 = -4.5 soft (a larger reward, not a penalty)");
    std::printf("\n");
}

template <typename S>
void randomWalk(const std::string& label) {
    S s = assignmentA<S>();
    optimaru::DecomposedObjective<S> calc;
    defaults<S>().into(calc);
    calc.rebuild(s);
    std::mt19937_64 rng(17);
    bool agree = true;
    for (int step = 0; step < 2000; ++step) {
        const bool trial = step % 3 == 0;
        const int t = static_cast<int>(rng() % 6);
        const int old = s.assignment[t];
        const int w = (rng() % 7 == 0) ? -1 : static_cast<int>(rng() % 4);
        if (trial) calc.begin_trial();
        calc.before_assign(t);
        s.assignment[t] = w;
        calc.after_assign(t);
        optimaru::DecomposedObjective<S> fresh;
        defaults<S>().into(fresh);
        fresh.rebuild(s);
        agree &= same(calc.score(), fresh.score());
        if (trial) {
            // Reject every third move: the snapshot must restore the medium
            // cells of the full re-cost constraint too.
            const Score candidate = calc.score();
            (void)candidate;
            calc.begin_rollback();
            calc.before_assign(t);
            s.assignment[t] = old;
            calc.after_assign(t);
            calc.end_trial(false);
            optimaru::DecomposedObjective<S> restored;
            defaults<S>().into(restored);
            restored.rebuild(s);
            agree &= same(calc.score(), restored.score());
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
    optimaru::DecomposedObjective<SoftFoldTasks> calc;
    defaults<SoftFoldTasks>().into(calc);
    calc.rebuild(s);
    calc.score();
    bool zeros = true;
    for (const auto& entry : calc.constraint_costs()) {
        if (entry.name == "conflictCost" || entry.name == "overloadCost" ||
            entry.name == "forbiddenWorkerCost") {
            for (double raw : entry.unit_costs) zeros &= raw == 0.0;
        }
        if (entry.name == "idleWorkerCost") zeros &= entry.unit_costs == std::vector<double>{1.0};
        if (entry.name == "overloadCost") zeros &= entry.level == Level::Medium && !entry.is_hard;
        if (entry.name == "preferenceReward") zeros &= entry.reward && !entry.level_flippable;
    }
    check(zeros, "raw cost 0 = no violation; constraintCosts() carry level/reward/flippable");
    check(calc.score().hard == 0, "no hard violation -> 0hard");
    randomWalk<SoftFoldTasks>("IntoSoft");
    randomWalk<HardFoldTasks>("BelowHard");
    std::printf("\n");
}

void testPythonName() {
    std::printf("== (h) pythonName ==\n");
    using optimaru::python::python_name;
    check(python_name("facilityCapacityCost") == "facility_capacity_cost",
          "facilityCapacityCost -> facility_capacity_cost");
    check(python_name("vehicleCapacityCost") == "vehicle_capacity_cost",
          "vehicleCapacityCost -> vehicle_capacity_cost");
    check(python_name("distanceFromFacilityCost") == "distance_from_facility_cost",
          "distanceFromFacilityCost -> distance_from_facility_cost");
    using optimaru::python::instance_keys;
    check(instance_keys({"day_pattern_cost", "min_rest_cost", "day_pattern_cost", "day_pattern_cost"}) ==
                  std::vector<std::string>{"day_pattern_cost", "min_rest_cost", "day_pattern_cost#2",
                                           "day_pattern_cost#3"},
          "instanceKeys: a repeated name's k-th instance is '<name>#<k>', a single one keeps its name");
    std::string refused;
    try {
        instance_keys({"min_rest_cost", "day_pattern_cost#2"});
    } catch (const std::invalid_argument& error) {
        refused = error.what();
    }
    check(refused == "constraint 'day_pattern_cost#2': a name must not contain '#', which "
                     "constraint_costs() keys repeated picks with ('<name>#<k>')",
          "instanceKeys refuses a name holding '#' (ValueError), in the Python base's words");
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
        check(throwsInvalid([&] { c.set_weight(-1.0); }) && c.weight == 1.0,
              "setWeight refuses a negative weight and leaves the weight alone");
        c.set_weight(2.5);
        check(c.weight == 2.5, "setWeight re-states the weight");
        check(throwsInvalid([&] { c.set_exponent(0.0); }) && c.exponent == 1.0,
              "setExponent refuses 0");
        c.set_level(Level::Hard);
        check(c.level == Level::Hard, "setLevel moves a flippable constraint");
        PreferenceReward<SoftFoldTasks> reward;
        check(throwsInvalid([&] { reward.set_level(Level::Hard); }), "setLevel(Hard) on a reward throws");
        ForbiddenWorkerCost<SoftFoldTasks> forbidden;
        check(throwsInvalid([&] { forbidden.set_level(Level::Soft); }),
              "setLevel on a non-flippable constraint throws");
        forbidden.set_level(Level::Hard);
        check(forbidden.level == Level::Hard, "re-stating a non-flippable level is a no-op");
    }
    {
        // Two instances of one name with their own labels, the way a rule
        // factory adds them; the registry name stays the lookup key.
        SoftFoldTasks s = assignmentA<SoftFoldTasks>();
        optimaru::DecomposedObjective<SoftFoldTasks> calc;
        auto first = std::make_unique<TaskCostCost<SoftFoldTasks>>();
        first->label = "task cost (all workers)";
        auto second = std::make_unique<OnlyWorkerZero<SoftFoldTasks>>(
                std::make_unique<TaskCostCost<SoftFoldTasks>>(3.0));
        second->label = "task cost (worker 0)";
        calc.add(std::move(first));
        calc.add(std::move(second));
        calc.rebuild(s);
        const Score score = calc.score();
        check(same(score, {0, -(28.0 + 3.0 * 15.0)}), "both instances score: 28 + 3 x 15, got " + str(score));
        const auto totals = calc.constraint_totals();
        const auto costs = calc.constraint_costs();
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
        check(wrapped->reward && !wrapped->level_flippable && wrapped->weight == 0.5 &&
                      wrapped->level == Level::Soft && wrapped->name == "preferenceReward",
              "a wrapper copying the inner metadata keeps reward / levelFlippable / weight / level");
        check(throwsInvalid([&] { wrapped->set_level(Level::Hard); }),
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
        const auto rows = optimaru::catalog_info(defaultSet, catalog);
        check(rows.size() == 4 && rows[0].name == "conflictCost" && rows[1].name == "taskCostCost" &&
                      rows[2].name == "overloadCost" && rows[3].name == "forbiddenWorkerCost",
              "catalogInfo: the default rows first, then the catalog-only ones, each name once");
        check(rows[0].default_enabled && rows[1].default_enabled && !rows[2].default_enabled &&
                      !rows[3].default_enabled,
              "defaultEnabled: in the defaults at weight > 0 (not an opt-in weight 0, not catalog-only)");
        check(rows[3].default_level == Level::Hard && !rows[3].level_flippable,
              "a catalog-only row carries its component's own defaults");
        optimaru::ConstraintAssembly<SoftFoldTasks> drifted;
        drifted.emplace<ConflictCost<SoftFoldTasks>>(5.0);
        bool threw = false;
        try {
            optimaru::catalog_info(defaultSet, drifted);
        } catch (const std::logic_error&) {
            threw = true;
        }
        check(threw, "catalogInfo throws when the catalog declares a default differently");
        const auto plain = defaultSet.info();
        check(plain[0].default_enabled && !plain[2].default_enabled,
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
    using optimaru::python::checked_level;
    using optimaru::python::level_named;
    const MediumFold none = MediumFold::none();
    const MediumFold into_soft = MediumFold::into_soft(1000.0);
    const MediumFold below_hard = MediumFold::below_hard(100);
    check(level_named("hard") == Level::Hard && level_named("medium") == Level::Medium &&
                  level_named("soft") == Level::Soft && !level_named("Hard") && !level_named(""),
          "a level is named exactly 'hard', 'medium' or 'soft'");
    // What solve(..., constraints=[RewardCost(level="hard")]) and
    // StructuralCost(level="soft") raise as ValueError through apply_constraints /
    // apply_knobs, in the Python base's words.
    const PreferenceReward<SoftFoldTasks> reward;
    check(refusal([&] { checked_level(reward, Level::Hard, into_soft, "'preference_reward'"); }) ==
                  "constraint 'preference_reward' is a reward (raw cost <= 0) and cannot be hard",
          "level='hard' on a reward is refused: 'is a reward'");
    check(checked_level(reward, Level::Soft, into_soft, "'preference_reward'") == Level::Soft,
          "level='soft' on a soft reward re-states its level");
    check(refusal([&] { checked_level(reward, Level::Medium, below_hard, "'preference_reward'"); }) ==
                  "constraint 'preference_reward' is a reward (raw cost <= 0) and cannot be "
                  "medium here, where medium counts toward the hard score",
          "level='medium' on a reward is refused where medium lands in the hard level");
    const ForbiddenWorkerCost<SoftFoldTasks> forbidden;
    check(refusal([&] { checked_level(forbidden, Level::Soft, into_soft, "'forbidden_worker_cost'"); }) ==
                  "constraint 'forbidden_worker_cost' is always hard; level='soft' is not allowed",
          "level='soft' on a non-flippable hard constraint is refused: 'is always hard'");
    check(refusal([&] { checked_level(forbidden, Level::Medium, into_soft, "'forbidden_worker_cost'"); }) ==
                  "constraint 'forbidden_worker_cost' is always hard; level='medium' is not allowed",
          "level='medium' on it is refused too");
    check(checked_level(forbidden, Level::Hard, into_soft, "'forbidden_worker_cost'") == Level::Hard,
          "level='hard' on it re-states its level");
    const OverloadCost<SoftFoldTasks> overload;
    check(checked_level(overload, Level::Soft, into_soft, "'overload_cost'") == Level::Soft &&
                  checked_level(overload, Level::Hard, into_soft, "'overload_cost'") == Level::Hard &&
                  checked_level(overload, Level::Medium, into_soft, "'overload_cost'") == Level::Medium,
          "a flippable medium constraint takes every level");
    check(refusal([&] { checked_level(overload, Level::Medium, none, "'overload_cost'"); }) ==
                  "constraint 'overload_cost': this use case has no medium level",
          "level='medium' on a domain without a medium fold is refused");
    OverloadCost<SoftFoldTasks> fixedMedium;
    fixedMedium.level_flippable = false;
    check(refusal([&] { checked_level(fixedMedium, Level::Soft, into_soft, "'overload_cost'"); }) ==
                          "constraint 'overload_cost' is always medium; level='soft' is not allowed" &&
                  refusal([&] { checked_level(fixedMedium, Level::Hard, into_soft, "'overload_cost'"); }) ==
                          "constraint 'overload_cost' is always medium; level='hard' is not allowed",
          "a non-flippable medium constraint refuses level='hard' and level='soft'");
    std::printf("\n");
}

// --- (k) sequence shape with a medium level -------------------------------------

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
    void reset(const S&, const std::vector<std::vector<int>>& members_per_unit) override {
        sizes.clear();
        for (const auto& m : members_per_unit) sizes.push_back(static_cast<int>(m.size()));
    }
    void on_unit_changed(const S&, int unit, const std::vector<int>& members) override {
        sizes[unit] = static_cast<int>(members.size());
    }
    double cost() const override {
        return static_cast<double>(*std::max_element(sizes.begin(), sizes.end()));
    }
    bool supports_preview() const override { return true; }
    double preview_cost(const S&, const optimaru::PreviewedUnit* changes, int count) const override {
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
    optimaru::DecomposedObjective<S> calc;
    routeDefaults<S>()
            .weight("routeOverflowCost", mediumWeight)
            .weight("longestRouteCost", mediumWeight)
            .into(calc);
    optimaru::Editor<S> editor(s, calc, optimaru::ScoreChecks::Full);
    editor.score();
    // By hand: medium cells 3w, 1w, 0 and the global 10w. BelowHard rounds
    // each cell and the global like hard, so at w = 0.6 that is
    // llround(1.8) + llround(0.6) + llround(6.0) = 9, not llround(14 x 0.6) = 8.
    const double w = mediumWeight;
    const double expected =
            optimaru::MediumFoldOf<S>::value.rounds_medium()
                    ? static_cast<double>(std::llround(3 * w) + std::llround(w) + std::llround(10 * w))
                    : 14.0 * w;
    const double medium = calc.level_scores().medium;  // a score: -penalty
    check(std::fabs(medium + expected) < 1e-9,
          label + ": the medium penalty at the start is " + std::to_string(expected) + ", got " +
                  std::to_string(-medium));
    optimaru::testing::PreviewParity parity;
    bool corrupted = false;
    try {
        parity = optimaru::testing::run_preview_parity(editor, calc, seed, 4000);
    } catch (const optimaru::ScoreMismatchError& error) {
        corrupted = true;
        std::printf("    %s\n", error.what());
    }
    std::printf("    %d previews checked, %d declined\n", parity.checked, parity.declined);
    check(!corrupted && parity.all_equal() && parity.declined == 0 && parity.checked > 1000,
          label + ": every preview equals the applied score (ScoreChecks::Full: == a full recompute)");
}

void testListMediumPreviews() {
    std::printf("== (k) sequence shape with a medium level: previews ==\n");
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
Score busyScore(double weight, std::vector<int> assignment) {
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
    optimaru::DecomposedObjective<S> calc;
    {
        optimaru::ConstraintAssembly<S> assembly;
        assembly.template emplace<BusyWorkerCost<S>>(weight);
        assembly.template emplace<ConflictCost<S>>();
        assembly.into(calc);
    }
    calc.rebuild(s);
    std::mt19937 rng(seed);
    bool agree = true;
    for (int step = 0; step < 400; ++step) {
        const int task = static_cast<int>(rng() % 6);
        const int worker = static_cast<int>(rng() % 4);
        calc.before_assign(task);
        s.assignment[task] = worker;
        calc.after_assign(task);
        optimaru::DecomposedObjective<S> fresh;
        optimaru::ConstraintAssembly<S> assembly;
        assembly.template emplace<BusyWorkerCost<S>>(weight);
        assembly.template emplace<ConflictCost<S>>();
        assembly.into(fresh);
        fresh.rebuild(s);
        agree &= calc.score() == fresh.score();
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
    const Score three = busyScore<Tasks>(4e18, {0, 0, 1, 1, 2, 2});
    check(three.hard == -kLimit,
          "three busy workers at 4e18 saturate at -INT64_MAX, got " + str(three));
    check(busyScore<Tasks>(4e18, {0, 1, 2, 3, 0, 1}).hard == -kLimit,
          "four busy workers saturate too (worse never looks better)");
    // BelowHard(100): 4e16 hard units per busy worker; the fold multiplies by 100.
    check(busyScore<HardFoldTasks>(4e16, {0, 0, 0, 0, 0, 0}).hard == -4000000000000000000LL,
          "BelowHard: one busy worker at 4e16 folds exactly (x100)");
    check(busyScore<HardFoldTasks>(4e16, {0, 0, 0, 1, 1, 1}).hard == -8000000000000000000LL,
          "BelowHard: two busy workers at 4e16 fold exactly (8e18 fits)");
    const Score folded = busyScore<HardFoldTasks>(4e16, {0, 0, 1, 1, 2, 2});
    check(folded.hard == -kLimit,
          "BelowHard: three busy workers at 4e16 fold past int64 -> -INT64_MAX, got " + str(folded));
    check(kHardFold.to_engine(-100000000000000000LL, -5.0, 0.0).hard == -kLimit,
          "MediumFold::to_engine(BelowHard) saturates like the objective");
    check(kHardFold.to_engine(-3, -5.0, 0.0).hard == -305,
          "MediumFold::to_engine(BelowHard) is exact in range");
    check(saturatingWalk<Tasks>(3e18, 61), "incremental == fresh across saturation (fold-less)");
    check(saturatingWalk<HardFoldTasks>(3e16, 67), "incremental == fresh across saturation (BelowHard)");

    // HardTotal: wraps and comes back exactly.
    optimaru::HardTotal total;
    for (int i = 0; i < 5; ++i) total.add(4000000000000000000LL);
    check(!total.fits() && total.saturated() == kLimit && total.positive(),
          "HardTotal: 5 x 4e18 does not fit and saturates at +INT64_MAX");
    for (int i = 0; i < 5; ++i) total.subtract(4000000000000000000LL);
    check(total.is_zero() && total.fits(), "HardTotal: subtracting them again is exact (0)");

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
    Score weightZero;
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
        // a medium global's total in level_scores() / constraint_totals().
        HardFoldTasks h;
        h.assignment = {0, 0, 0, 0, 0, 0};  // 3 idle workers: 3^2000 = inf
        optimaru::ConstraintAssembly<HardFoldTasks> assembly;
        assembly.emplace<IdleWorkerCost<HardFoldTasks>>(0.0);
        assembly.exponent("idleWorkerCost", 2000.0);
        assembly.emplace<OverloadCost<HardFoldTasks>>(0.0);
        assembly.exponent("overloadCost", 2000.0);  // 4 tasks over: 4^2000 = inf
        assembly.emplace<TaskCostCost<HardFoldTasks>>();
        optimaru::DecomposedObjective<HardFoldTasks> calc;
        assembly.into(calc);
        bool threw = false;
        bool allZero = true;
        double soft = 0.0;
        try {
            calc.rebuild(h);
            const auto levels = calc.level_scores();
            allZero = levels.hard == 0 && levels.medium == 0.0;
            for (const auto& total : calc.constraint_totals()) {
                if (total.name != "taskCostCost" && total.cost != 0.0) allZero = false;
            }
            calc.before_assign(5);
            h.assignment[5] = 1;
            calc.after_assign(5);
            const Score moved = calc.score();
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
        beyond = error.beyond_hard_limit();
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
        beyond = error.beyond_hard_limit();
    }
    check(message.find("is not finite (inf)") != std::string::npos && !beyond,
          "a non-finite hard contribution is reported as not finite: " + message);
    check(optimaru::ContributionOverflowError::message("conflict_cost", "3e+19", true) ==
                  "constraint 'conflict_cost': a hard contribution of 3e+19 exceeds the engine's "
                  "limit of 2**62 (about 4.6e18) per unit; lower its weight or exponent",
          "the hard-limit message reads like the Python mirror's");
    std::printf("\n");
}

// --- (m) restate_knobs ------------------------------------------------------

void testRestateKnobs() {
    std::printf("== (m) python::restateKnobs: the knob rule of both binding paths ==\n");
    using optimaru::python::Knobs;
    using optimaru::python::restate_knobs;
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
        restate_knobs(assembly, "taskCostCost", knobs, "'task_cost_cost'");
        check(assembly.find("taskCostCost")->exponent == 1.0,
              "an explicit exponent 1.0 resets a component built with exponent 2");
        check(scoreOf(std::move(assembly), t).soft == -28.0,
              "...and the assembly scores unshaped: soft -28 (not -(15^2 + 11^2 + 2^2))");
    }
    {
        auto assembly = shaped();
        restate_knobs(assembly, "taskCostCost", Knobs{}, "'task_cost_cost'");
        check(assembly.find("taskCostCost")->exponent == 1.0,
              "an unset exponent (None) reads as 1.0 and resets it too");
    }
    {
        auto assembly = shaped();
        Knobs knobs;
        knobs.exponent = 3.0;
        restate_knobs(assembly, "taskCostCost", knobs, "'task_cost_cost'");
        check(assembly.find("taskCostCost")->exponent == 3.0, "exponent 3 is applied");
    }
    {
        auto assembly = defaults<SoftFoldTasks>();
        Knobs knobs;
        knobs.weight = 5.0;
        knobs.level = Level::Soft;
        restate_knobs(assembly, "conflictCost", knobs, "'conflict_cost'");
        const optimaru::ConstraintMetadata* conflict = assembly.find("conflictCost");
        check(conflict->weight == 5.0 && conflict->level == Level::Soft,
              "weight 5, level soft: re-stated on the assembly");
        Knobs keep;  // weight / level unset keep the component's defaults
        restate_knobs(assembly, "overloadCost", keep, "'overload_cost'");
        const optimaru::ConstraintMetadata* overload = assembly.find("overloadCost");
        check(overload->weight == 1.0 && overload->level == Level::Medium,
              "weight / level unset keep the default weight and the medium level");
        Knobs hard;
        hard.level = Level::Hard;
        check(refusal([&] {
                  restate_knobs(assembly, "preferenceReward", hard, "'preference_reward'");
              }) == "constraint 'preference_reward' is a reward (raw cost <= 0) and cannot be hard",
              "level='hard' on a reward is refused in the Python base's words");
        Knobs medium;
        medium.level = Level::Medium;
        restate_knobs(assembly, "taskCostCost", medium, "'task_cost_cost'");
        check(assembly.find("taskCostCost")->level == Level::Medium,
              "level='medium' moves a soft constraint onto the medium level");
    }
    {
        // apply_knobs' path: a component built outside any assembly.
        TaskCostCost<Tasks> bare;
        bare.set_exponent(2.0);
        Knobs knobs;
        knobs.exponent = 1.0;
        restate_knobs(bare, knobs, optimaru::MediumFold::none(), "'task_cost_cost'");
        check(bare.exponent == 1.0,
              "the component path applies an explicit 1.0 the same way as the assembly path");
        Knobs soft;
        soft.level = Level::Soft;
        soft.weight = 2.5;
        ForbiddenWorkerCost<Tasks> forbidden;
        check(refusal([&] { restate_knobs(forbidden, soft, kSoftFold, "'forbidden_worker_cost'"); }) ==
                      "constraint 'forbidden_worker_cost' is always hard; level='soft' is not allowed",
              "level='soft' on a non-flippable component is refused on the component path too");
    }
    std::printf("\n");
}

// --- (n) default weight / level in constraint_costs() --------------------------

void testReportedDefaults() {
    std::printf("== (n) constraintCosts(): default weight / level next to the re-stated ones ==\n");
    SoftFoldTasks s = assignmentA<SoftFoldTasks>();
    auto assembly = defaults<SoftFoldTasks>();
    assembly.weight("conflictCost", 5.0).soft("conflictCost");
    assembly.hard("overloadCost");
    assembly.weight("preferenceReward", 2.0);
    optimaru::DecomposedObjective<SoftFoldTasks> calc;
    assembly.into(calc);
    calc.rebuild(s);
    calc.score();
    bool conflict = false;
    bool overload = false;
    bool reward = false;
    bool untouched = true;
    for (const auto& entry : calc.constraint_costs()) {
        if (entry.name == "conflictCost") {
            conflict = entry.weight == 5.0 && entry.level == Level::Soft &&
                       entry.default_weight == 1.0 && entry.default_level == Level::Hard;
        } else if (entry.name == "overloadCost") {
            overload = entry.level == Level::Hard && entry.default_level == Level::Medium;
        } else if (entry.name == "preferenceReward") {
            reward = entry.weight == 2.0 && entry.default_weight == 0.5;
        } else if (entry.default_weight != entry.weight || entry.default_level != entry.level) {
            untouched = false;
        }
    }
    check(conflict, "conflictCost re-stated to weight 5 / soft reports default 1 / hard");
    check(overload, "overloadCost made hard reports its default level medium");
    check(reward, "preferenceReward at weight 2 reports its default weight 0.5");
    check(untouched, "a component left alone reports default == current");
    // A wrapper copying its inner component's metadata takes the defaults too.
    TaskCostCost<Tasks> inner(4.0);
    inner.set_weight(9.0);
    TaskCostCost<Tasks> copy;
    static_cast<optimaru::ConstraintMetadata&>(copy) = inner;
    check(copy.weight == 9.0 && copy.default_weight == 4.0,
          "a metadata copy carries the inner component's construction weight as its default");
    std::printf("\n");
}


// --- (o) offset / clip ----------------------------------------------------

// A scoped unit constraint: one raw unit per task on workers 0 and 2 only
// (scoped_units() = {2, 0}); the other cells are never evaluated.
template <typename S>
struct ScopedLoadCost final : optimaru::UnitConstraint<S> {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr Level kDefaultLevel = Level::Soft;
    explicit ScopedLoadCost(double weight = kDefaultWeight)
        : optimaru::UnitConstraint<S>("scopedLoadCost", weight, kDefaultLevel) {}
    std::vector<int> scope{2, 0};
    const std::vector<int>* scoped_units() const override { return &scope; }
    double cost(const S&, int unit, const std::vector<int>& members) const override {
        return unit == 0 || unit == 2 ? static_cast<double>(members.size()) : 0.0;
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const S&, int unit, const std::vector<int>&, int) const override {
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
    using optimaru::shape_cost;
    // The formula itself.
    check(shape_cost(15.0, 1.0, 5.0, true) == 10.0 && shape_cost(3.0, 1.0, 5.0, true) == 0.0 &&
                  shape_cost(0.0, 2.0, 5.0, true) == 0.0 && shape_cost(-8.0, 2.0, 5.0, true) == -9.0,
          "threshold: sign(x) * max(0, |x| - offset)^e (15 -> 10, 3 -> 0, 0 -> 0, -8 -> -9 at e 2)");
    check(shape_cost(15.0, 3.0, 10.0, false) == 125.0 && shape_cost(7.0, 3.0, 10.0, false) == -27.0 &&
                  shape_cost(0.0, 1.0, 10.0, false) == -10.0,
          "literal: shape(x - offset, e) ((15-10)^3 = 125, (7-10)^3 = -27, a raw 0 -> -10)");
    check(shape_cost(4.0, 2.0, -1.0, true) == 25.0 && shape_cost(0.0, 2.0, -1.0, true) == 0.0 &&
                  shape_cost(-4.0, 1.0, -1.0, true) == -5.0 && shape_cost(0.0, 2.0, -1.0, false) == 1.0,
          "a negative offset: threshold adds |offset| to every non-zero x, literal to every x");
    bool samePlain = true;
    for (double x : {-7.5, -1.0, -0.0, 0.0, 0.3, 1.0, 2.0, 17.25, 1e10}) {
        for (double e : {0.5, 1.0, 2.0, 3.0}) {
            for (bool clip : {true, false}) {
                const double a = shape_cost(x, e, 0.0, clip);
                const double b = shape_cost(x, e);
                samePlain &= std::memcmp(&a, &b, sizeof a) == 0;
            }
        }
    }
    check(samePlain, "offset 0 is the plain shape_cost bit for bit, in both modes");
    check(optimaru::weighted_cost(0.0, 0.0, 400.0, 10.0, false) == 0.0 &&
                  optimaru::weighted_cost(0.0, 1e300, 50.0, -3.0, true) == 0.0,
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
    // Hard rounding per unit: conflictCost [2, 1, 0, 0], threshold offset
    // 0.5 -> [1.5, 0.5, 0, 0], llround 2 + 1 = 3 (not llround(2.0) = 2).
    check(scoreOf(one<Tasks>("conflictCost").offset("conflictCost", 0.5), t).hard == -3,
          "threshold offset 0.5 on a hard constraint: rounded per unit, half away from zero");
    // The literal formula on hard is refused at reset (section (q)); on soft
    // it keeps its numbers: [1, 0, -1, -1] = -1 soft (a reward below 1).
    check(throwsLogic(one<Tasks>("conflictCost").offset("conflictCost", 1.0).clip("conflictCost", false),
                      t),
          "literal offset 1 on a hard constraint: refused at reset");
    check(softOf(one<Tasks>("conflictCost")
                         .soft("conflictCost")
                         .offset("conflictCost", 1.0)
                         .clip("conflictCost", false),
                 t) == -1.0,
          "literal offset 1 on it moved to soft: [1, 0, -1, -1] = -1 soft");
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
        optimaru::DecomposedObjective<Tasks> calc;
        scoped.into(calc);
        calc.rebuild(t);
        // Worker 0 holds 3 tasks (3 - 1 = 2), worker 2 none (0 - 1 = -1);
        // workers 1 and 3 are not evaluated.
        check(-calc.score().soft == 1.0,
              "literal offset 1 on a scoped constraint: only workers 0 and 2 count (2 - 1 = 1)");
        const auto entries = calc.constraint_costs();
        check(entries.size() == 1 && entries[0].scoped &&
                      entries[0].units == std::vector<int>{0, 2} &&
                      entries[0].unit_costs == std::vector<double>{3.0, 0.0, 0.0, 0.0} &&
                      entries[0].offset == 1.0 && !entries[0].clip,
              "constraintCosts(): a scoped constraint reports its evaluated units, sorted, and "
              "the offset / clip");
    }
    {
        optimaru::DecomposedObjective<Tasks> calc;
        defaults<Tasks>().soft("overloadCost").soft("idleWorkerCost").into(calc);
        calc.rebuild(t);
        calc.score();
        bool plain = true;
        for (const auto& entry : calc.constraint_costs()) {
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
        optimaru::python::restate_knobs(assembly, "taskCostCost", Knobs{}, "'task_cost_cost'");
        const optimaru::ConstraintMetadata* reset = assembly.find("taskCostCost");
        check(reset->offset == 0.0 && reset->clip,
              "restateKnobs: an unset offset / clip reads as 0 / true and resets the component");
        Knobs knobs;
        knobs.offset = -2.0;
        knobs.clip = false;
        TaskCostCost<Tasks> bare;
        optimaru::python::restate_knobs(bare, knobs, optimaru::MediumFold::none(),
                                       "'task_cost_cost'");
        check(bare.offset == -2.0 && !bare.clip, "restateKnobs applies offset / clip to a component");
    }
    {
        // An assembly with offset 0 scores exactly like the plain shaping.
        Tasks a = assignmentA<Tasks>();
        auto plain = [] { return defaults<Tasks>().soft("overloadCost").soft("idleWorkerCost"); };
        const Score before = scoreOf(plain(), a);
        const Score restated =
                scoreOf(plain().offset("taskCostCost", 0.0).clip("taskCostCost", false), a);
        check(before == restated && before.hard == -4 && before.soft == -28.5,
              "offset 0 with clip false scores exactly like the plain shaping (-4hard/-28.5soft)");
    }
    std::printf("\n");
}

// --- (p) several instances of one name ------------------------------------

template <typename S>
optimaru::ConstraintAssembly<S> repeated() {
    // taskCostCost three times: as is, exponent 2 with the threshold at 5, and
    // the literal formula at 10 at weight 0.5; conflictCost twice (the copy
    // moved to soft with the literal offset 1, which hard refuses);
    // idleWorkerCost twice (the copy shaped on its total, literally: on
    // medium where medium folds into soft, on soft where it counts toward
    // the hard score); overloadCost (full re-cost) twice, the copy shifted.
    auto assembly = defaults<S>();
    assembly.add_instances(
            one<S>("taskCostCost").exponent("taskCostCost", 2.0).offset("taskCostCost", 5.0));
    assembly.add_instances(one<S>("taskCostCost")
                                  .weight("taskCostCost", 0.5)
                                  .offset("taskCostCost", 10.0)
                                  .clip("taskCostCost", false));
    assembly.add_instances(one<S>("conflictCost")
                                  .soft("conflictCost")
                                  .offset("conflictCost", 1.0)
                                  .clip("conflictCost", false));
    auto idle = one<S>("idleWorkerCost");
    idle.offset("idleWorkerCost", 1.0).exponent("idleWorkerCost", 2.0).clip("idleWorkerCost", false);
    if constexpr (optimaru::MediumFoldOf<S>::value.rounds_medium()) idle.soft("idleWorkerCost");
    assembly.add_instances(std::move(idle));
    assembly.add_instances(one<S>("overloadCost").offset("overloadCost", -1.0).weight("overloadCost", 2.0));
    return assembly;
}

template <typename S, typename Make>
bool walkAgrees(Make make, unsigned seed) {
    S s = assignmentA<S>();
    optimaru::DecomposedObjective<S> calc;
    make().into(calc);
    calc.rebuild(s);
    std::mt19937_64 rng(seed);
    bool agree = true;
    auto fresh = [&] {
        optimaru::DecomposedObjective<S> other;
        make().into(other);
        other.rebuild(s);
        return other.score();
    };
    for (int step = 0; step < 2000; ++step) {
        const bool trial = step % 3 == 0;
        const int task = static_cast<int>(rng() % 6);
        const int old = s.assignment[task];
        const int worker = (rng() % 7 == 0) ? -1 : static_cast<int>(rng() % 4);
        if (trial) calc.begin_trial();
        calc.before_assign(task);
        s.assignment[task] = worker;
        calc.after_assign(task);
        agree &= same(calc.score(), fresh());
        if (trial) {
            calc.begin_rollback();
            calc.before_assign(task);
            s.assignment[task] = old;
            calc.after_assign(task);
            calc.end_trial(false);
            agree &= same(calc.score(), fresh());
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
    bool has_integral_raw_deltas() const override { return true; }
    void on_reset(const S&) override {}
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
    bool supports_delta() const override { return true; }
    double delta_cost(const S& s, int unit, const std::vector<int>&, int entity) const override {
        return static_cast<double>(s.cost[entity][unit]);
    }
    const char* delta_group_key() const override { return "test.grouped-task"; }
    std::unique_ptr<optimaru::UnitConstraintDeltaGroup<S>> create_delta_group() const override {
        return std::make_unique<TaskDeltaGroup<S>>();
    }
};

template <typename S>
optimaru::ConstraintAssembly<S> groupedRepeats() {
    optimaru::ConstraintAssembly<S> assembly;
    assembly.template emplace<GroupedTaskCost<S>>();
    assembly.template emplace_instance<GroupedTaskCost<S>>();
    assembly.template emplace_instance<GroupedTaskCost<S>>(2.0);
    auto shifted = std::make_unique<GroupedTaskCost<S>>();
    shifted->set_level(Level::Soft);  // the literal formula is refused on hard
    shifted->set_offset(9.0);
    shifted->set_clip(false);
    assembly.add_instance(std::move(shifted));
    auto threshold = std::make_unique<GroupedTaskCost<S>>(3.0);
    threshold->set_offset(10.0);
    assembly.add_instance(std::move(threshold));
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
    // The literal formula stays off the levels that count toward the hard
    // score: the hard routeDemandCost copy moves to soft, and so does the
    // medium longestRouteCost copy where medium folds below hard.
    if constexpr (optimaru::MediumFoldOf<S>::value.rounds_medium()) copy.soft("longestRouteCost");
    copy.soft("routeDemandCost")
            .offset("routeDemandCost", 2.0)
            .clip("routeDemandCost", false)
            .weight("routeDemandCost", 0.5);
    assembly.add_instances(std::move(copy));
    return assembly;
}

template <typename S>
void listRepeatPreviews(const std::string& label, std::uint64_t seed) {
    S s = routesStart<S>();
    optimaru::DecomposedObjective<S> calc;
    routeRepeats<S>().into(calc);
    optimaru::Editor<S> editor(s, calc, optimaru::ScoreChecks::Full);
    editor.score();
    optimaru::testing::PreviewParity parity;
    bool corrupted = false;
    try {
        parity = optimaru::testing::run_preview_parity(editor, calc, seed, 4000);
    } catch (const optimaru::ScoreMismatchError& error) {
        corrupted = true;
        std::printf("    %s\n", error.what());
    }
    std::printf("    %d previews checked, %d declined\n", parity.checked, parity.declined);
    check(!corrupted && parity.all_equal() && parity.declined == 0 && parity.checked > 1000,
          label + ": repeated, offset picks: every preview equals the applied score (ScoreChecks::Full)");
}

void testInstances() {
    std::printf("== (p) Several instances of one name ==\n");
    Tasks t = assignmentA<Tasks>();
    // By hand on A: taskCostCost [15, 11, 0, 2] = 28; the copy with exponent
    // 2 and the threshold at 5: 10^2 + 6^2 = 136; together 164.
    {
        auto assembly = one<Tasks>("taskCostCost");
        assembly.emplace_instance<TaskCostCost<Tasks>>();
        assembly.exponent("taskCostCost", 2.0);  // the FIRST instance only
        check(assembly.size() == 2 && assembly.names() ==
                                              std::vector<std::string>{"taskCostCost", "taskCostCost"},
              "emplaceInstance keeps the instance already held");
        check(softOf(std::move(assembly), t) == 15 * 15 + 11 * 11 + 2 * 2 + 28.0,
              "a by-name edit addresses the first instance: 354 (shaped) + 28 (the copy)");
    }
    {
        auto assembly = one<Tasks>("taskCostCost");
        assembly.add_instances(
                one<Tasks>("taskCostCost").exponent("taskCostCost", 2.0).offset("taskCostCost", 5.0));
        check(softOf(std::move(assembly), t) == 28.0 + 136.0,
              "x + max(0, x - 5)^2 per unit: 28 + 136 = 164 (two instances, each its own knobs)");
    }
    {
        auto assembly = one<Tasks>("taskCostCost");
        assembly.add_instances(one<Tasks>("taskCostCost"));
        check(softOf(std::move(assembly), t) == 56.0, "identical instances simply add up (2 x 28)");
    }
    {
        auto assembly = one<Tasks>("taskCostCost");
        assembly.emplace_instance<TaskCostCost<Tasks>>(3.0);
        assembly.drop("taskCostCost");
        check(assembly.size() == 1 && assembly.find("taskCostCost")->weight == 3.0,
              "drop() removes the first instance; the second becomes the first");
        auto both = defaults<Tasks>();
        both.emplace_instance<TaskCostCost<Tasks>>(3.0);
        both.only({"taskCostCost"});
        check(both.size() == 2, "only() keeps every instance of the names it is given");
    }
    {
        optimaru::DecomposedObjective<SoftFoldTasks> calc;
        repeated<SoftFoldTasks>().into(calc);
        SoftFoldTasks a = assignmentA<SoftFoldTasks>();
        calc.rebuild(a);
        calc.score();
        std::vector<std::string> names;
        for (const auto& entry : calc.constraint_costs()) names.push_back(entry.name);
        const std::vector<std::string> keys = optimaru::python::instance_keys(names);
        check(keys == std::vector<std::string>{"conflictCost", "overloadCost", "taskCostCost",
                                               "preferenceReward", "forbiddenWorkerCost",
                                               "taskCostCost#2", "taskCostCost#3", "conflictCost#2",
                                               "overloadCost#2", "idleWorkerCost",
                                               "idleWorkerCost#2"},
              "constraintCosts(): each name's instances in the order they were added -> name, "
              "name#2, ...");
        bool ownKnobs = false;
        for (const auto& entry : calc.constraint_costs()) {
            if (entry.name == "taskCostCost" && entry.offset == 10.0) {
                ownKnobs = !entry.clip && entry.weight == 0.5 &&
                           entry.unit_costs == std::vector<double>{15.0, 11.0, 0.0, 2.0};
            }
        }
        check(ownKnobs, "a copy reports its own knobs and the same raw costs as the first");
        // By hand, on A (IntoSoft 1000): the defaults give -4hard /
        // -(26.5 + 2 x 1000)soft. The copies add: taskCostCost#2 136,
        // taskCostCost#3 0.5 x (5 + 1 - 10 - 8) = -6, conflictCost#2 (soft)
        // [1, 0, -1, -1] = -1 soft, overloadCost#2 2 x (1 + 1) = 4 medium
        // (threshold at -1: only worker 0 is non-zero), idleWorkerCost#2
        // shape(1 - 1, 2) = 0 medium.
        const Score score = calc.score();
        check(same(score, {-4, -(26.5 + 136.0 - 6.0 - 1.0 + (2 + 4) * 1000.0)}),
              "the repeated assembly by hand: -4hard / -6155.5soft, got " + str(score));
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
                      assembly.emplace_instance<ScopedLoadCost<Tasks>>(2.0);
                      assembly.emplace_instance<ConflictCost<Tasks>>();
                      return assembly;
                  },
                  31),
          "a scoped literal-offset constraint and its copy: incremental == fresh");
    {
        // By hand on A: groupedTaskCost per worker [15, 11, 0, 2] = 28, hard.
        // Instances: 28 + 28 + 2 x 28 + 3 x threshold at 10 ([5, 1, 0, 0] =
        // 6, x3 = 18) = 130 hard; the literal one at 9, on soft:
        // [6, 2, -9, -7] = -8 soft.
        optimaru::DecomposedObjective<Tasks> calc;
        groupedRepeats<Tasks>().into(calc);
        Tasks a = assignmentA<Tasks>();
        calc.rebuild(a);
        const Score score = calc.score();
        check(calc.delta_group_count() == 1 && same(score, {-130, 8.0}),
              "five instances of a grouped component share one delta group: -130hard / 8soft, got " +
                      str(score));
    }
    check(walkAgrees<Tasks>([] { return groupedRepeats<Tasks>(); }, 37),
          "grouped deltas: the direct hard fold for the unshaped instances, the shaped path for "
          "the offset ones; incremental == fresh");
    listRepeatPreviews<SoftFoldRoutes>("sequence IntoSoft", 59);
    listRepeatPreviews<HardFoldRoutes>("sequence BelowHard", 61);
    std::printf("\n");
}

// --- (q) the literal offset formula stays off the hard score ---------------

// The std::logic_error a reset throws (validateLevel), or "" when it scores.
template <typename S>
std::string resetRefusal(optimaru::ConstraintAssembly<S> assembly) {
    S s = assignmentA<S>();
    try {
        scoreOf(std::move(assembly), s);
    } catch (const std::logic_error& error) {
        return error.what();
    }
    return "";
}

std::string engineMessage(const std::string& name) {
    return "Constraint '" + name +
           "' cannot use clip=false with a non-zero offset on a level that counts toward the "
           "hard score (hard 0 must mean no violation)";
}

std::string hardMessage(const std::string& quoted) {
    return "constraint " + quoted +
           ": clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
           "violation; use clip=True (a threshold) or another level";
}

std::string mediumMessage(const std::string& quoted) {
    return "constraint " + quoted +
           ": clip=False with a non-zero offset cannot be medium here, where medium counts "
           "toward the hard score; use clip=True (a threshold) or level='soft'";
}

void testLiteralOffsetGuard() {
    std::printf("== (q) clip=false with an offset: refused where it counts toward the hard score ==\n");
    using optimaru::python::check_literal_offset;
    using optimaru::python::Knobs;
    using optimaru::python::restate_knobs;
    const MediumFold none = MediumFold::none();
    check(none.counts_toward_hard(Level::Hard) && !none.counts_toward_hard(Level::Soft) &&
                  kSoftFold.counts_toward_hard(Level::Hard) &&
                  !kSoftFold.counts_toward_hard(Level::Medium) &&
                  !kSoftFold.counts_toward_hard(Level::Soft) &&
                  kHardFold.counts_toward_hard(Level::Hard) &&
                  kHardFold.counts_toward_hard(Level::Medium) &&
                  !kHardFold.counts_toward_hard(Level::Soft),
          "countsTowardHard: hard always, medium only where it folds below hard");

    // The binding helper, on a component's effective state, in the Python
    // base's words.
    for (const double offset : {2.0, -2.0}) {
        const std::string sign = offset > 0 ? "offset 2" : "offset -2";
        ConflictCost<Tasks> conflict;  // default level hard, flippable
        conflict.set_offset(offset);
        conflict.set_clip(false);
        check(refusal([&] { check_literal_offset(conflict, none, "'conflict_cost'"); }) ==
                      hardMessage("'conflict_cost'"),
              "checkLiteralOffset, " + sign + ": the default-level hard constraint is refused");
        ForbiddenWorkerCost<Tasks> forbidden;  // structural hard
        forbidden.set_offset(offset);
        forbidden.set_clip(false);
        check(refusal([&] { check_literal_offset(forbidden, kSoftFold, "'forbidden_worker_cost'"); }) ==
                      hardMessage("'forbidden_worker_cost'"),
              "checkLiteralOffset, " + sign + ": a non-flippable hard constraint is refused");
        OverloadCost<Tasks> overload;  // default level medium
        overload.set_offset(offset);
        overload.set_clip(false);
        check(refusal([&] { check_literal_offset(overload, kHardFold, "'overload_cost'"); }) ==
                      mediumMessage("'overload_cost'"),
              "checkLiteralOffset, " + sign + ": medium below hard is refused, the medium message");
        check(refusal([&] { check_literal_offset(overload, kSoftFold, "'overload_cost'"); }).empty(),
              "checkLiteralOffset, " + sign + ": medium into soft is allowed");
        TaskCostCost<Tasks> soft;
        soft.set_offset(offset);
        soft.set_clip(false);
        check(refusal([&] { check_literal_offset(soft, kHardFold, "'task_cost_cost'"); }).empty(),
              "checkLiteralOffset, " + sign + ": soft is allowed");
        ConflictCost<Tasks> threshold;
        threshold.set_offset(offset);  // clip stays true
        check(refusal([&] { check_literal_offset(threshold, none, "'conflict_cost'"); }).empty(),
              "checkLiteralOffset, " + sign + ": clip=True on hard is allowed (a threshold)");
    }
    {
        ConflictCost<Tasks> unshifted;
        unshifted.set_clip(false);  // offset stays 0
        check(refusal([&] { check_literal_offset(unshifted, none, "'conflict_cost'"); }).empty(),
              "checkLiteralOffset: clip=False with offset 0 on hard is allowed (the plain shaping)");
    }

    // Both restate_knobs overloads check once every knob is re-stated, so the
    // effective level is what counts, whichever knob moves it.
    Knobs literal;
    literal.offset = 3.0;
    literal.clip = false;
    Knobs literalSoft = literal;
    literalSoft.level = Level::Soft;
    Knobs literalHard = literal;
    literalHard.level = Level::Hard;
    Knobs literalMedium = literal;
    literalMedium.level = Level::Medium;
    {
        auto assembly = defaults<SoftFoldTasks>();
        check(refusal([&] { restate_knobs(assembly, "conflictCost", literal, "'conflict_cost'"); }) ==
                      hardMessage("'conflict_cost'"),
              "restateKnobs (assembly): a literal offset on the default hard level is refused");
        check(refusal([&] {
                  restate_knobs(assembly, "forbiddenWorkerCost", literal, "'forbidden_worker_cost'");
              }) == hardMessage("'forbidden_worker_cost'"),
              "restateKnobs (assembly): ... and on a structural hard one");
        check(refusal([&] { restate_knobs(assembly, "taskCostCost", literalHard, "'task_cost_cost'"); }) ==
                      hardMessage("'task_cost_cost'"),
              "restateKnobs (assembly): level='hard' with a literal offset on a soft one is refused");
        check(refusal([&] { restate_knobs(assembly, "taskCostCost", literalMedium, "'task_cost_cost'"); })
                      .empty(),
              "restateKnobs (assembly): level='medium' with it where medium folds into soft is allowed");
    }
    {
        auto assembly = defaults<SoftFoldTasks>();
        check(refusal([&] { restate_knobs(assembly, "conflictCost", literalSoft, "'conflict_cost'"); })
                      .empty(),
              "restateKnobs (assembly): level='soft' with a literal offset on a hard one is allowed");
        const optimaru::ConstraintMetadata* conflict = assembly.find("conflictCost");
        check(conflict->level == Level::Soft && conflict->offset == 3.0 && !conflict->clip,
              "...and every knob of it is applied");
        SoftFoldTasks a = assignmentA<SoftFoldTasks>();
        check(!throwsLogic(std::move(assembly), a), "...and the objective accepts it at reset");
    }
    {
        auto assembly = defaults<HardFoldTasks>();
        check(refusal([&] { restate_knobs(assembly, "overloadCost", literal, "'overload_cost'"); }) ==
                      mediumMessage("'overload_cost'"),
              "restateKnobs (assembly): a literal offset on the default medium below hard is refused");
        check(refusal([&] { restate_knobs(assembly, "taskCostCost", literalMedium, "'task_cost_cost'"); }) ==
                      mediumMessage("'task_cost_cost'"),
              "restateKnobs (assembly): level='medium' with a literal offset below hard is refused");
    }
    {
        ConflictCost<Tasks> conflict;
        check(refusal([&] { restate_knobs(conflict, literal, none, "'conflict_cost'"); }) ==
                      hardMessage("'conflict_cost'"),
              "restateKnobs (component): a literal offset on the default hard level is refused");
        OverloadCost<HardFoldTasks> overload;
        check(refusal([&] { restate_knobs(overload, literal, kHardFold, "'overload_cost'"); }) ==
                      mediumMessage("'overload_cost'"),
              "restateKnobs (component): ... and on medium below hard, the medium message");
        OverloadCost<SoftFoldTasks> into_soft;
        check(refusal([&] { restate_knobs(into_soft, literal, kSoftFold, "'overload_cost'"); }).empty(),
              "restateKnobs (component): medium into soft is allowed");
        ConflictCost<Tasks> moved;
        check(refusal([&] { restate_knobs(moved, literalSoft, none, "'conflict_cost'"); }).empty() &&
                      moved.level == Level::Soft && !moved.clip,
              "restateKnobs (component): level='soft' with a literal offset is allowed");
        Knobs threshold;
        threshold.offset = -4.0;
        ConflictCost<Tasks> clipped;
        check(refusal([&] { restate_knobs(clipped, threshold, none, "'conflict_cost'"); }).empty(),
              "restateKnobs (component): a threshold offset on hard is allowed");
    }

    // The engine itself, at reset: the C++ API (ConstraintAssembly::offset /
    // clip / level in any order, and components set up directly).
    const std::string conflict = engineMessage("conflictCost");
    check(resetRefusal(one<Tasks>("conflictCost").offset("conflictCost", 2.0).clip("conflictCost", false)) ==
                  conflict,
          "reset refuses offset then clip(false) on a hard constraint, in the engine's words");
    check(resetRefusal(one<Tasks>("conflictCost").clip("conflictCost", false).offset("conflictCost", -2.0)) ==
                  conflict,
          "reset refuses clip(false) then a negative offset on a hard constraint");
    check(resetRefusal(one<Tasks>("taskCostCost")
                               .offset("taskCostCost", 2.0)
                               .clip("taskCostCost", false)
                               .hard("taskCostCost")) == engineMessage("taskCostCost"),
          "reset refuses a literal soft constraint moved to hard afterwards");
    check(resetRefusal(one<Tasks>("taskCostCost")
                               .hard("taskCostCost")
                               .clip("taskCostCost", false)
                               .offset("taskCostCost", 2.0)) == engineMessage("taskCostCost"),
          "reset refuses one moved to hard first");
    check(resetRefusal(one<Tasks>("forbiddenWorkerCost")
                               .offset("forbiddenWorkerCost", 1.0)
                               .clip("forbiddenWorkerCost", false)) ==
                  engineMessage("forbiddenWorkerCost"),
          "reset refuses it on a non-flippable hard constraint");
    check(resetRefusal(one<HardFoldTasks>("overloadCost")
                               .offset("overloadCost", 1.0)
                               .clip("overloadCost", false)) == engineMessage("overloadCost"),
          "reset refuses it on medium where medium folds below hard");
    check(resetRefusal(one<SoftFoldTasks>("overloadCost")
                               .offset("overloadCost", 1.0)
                               .clip("overloadCost", false))
                  .empty(),
          "reset allows it on medium where medium folds into soft");
    check(resetRefusal(one<Tasks>("conflictCost")
                               .offset("conflictCost", 2.0)
                               .clip("conflictCost", false)
                               .soft("conflictCost"))
                  .empty(),
          "reset allows a literal hard constraint moved to soft afterwards");
    check(resetRefusal(one<Tasks>("conflictCost").offset("conflictCost", 2.0)).empty() &&
                  resetRefusal(one<Tasks>("conflictCost").offset("conflictCost", -2.0)).empty(),
          "reset allows a threshold offset of either sign on hard");
    check(resetRefusal(one<Tasks>("conflictCost").clip("conflictCost", false)).empty(),
          "reset allows clip(false) with offset 0 on hard");
    {
        optimaru::DecomposedObjective<Tasks> calc;
        auto direct = std::make_unique<ConflictCost<Tasks>>();
        direct->set_clip(false);
        direct->set_offset(0.5);
        calc.add(std::move(direct));
        Tasks a = assignmentA<Tasks>();
        std::string message;
        try {
            calc.rebuild(a);
        } catch (const std::logic_error& error) {
            message = error.what();
        }
        check(message == conflict, "reset refuses a component set up directly and added by add()");
    }
    {
        // The metadata stays mutable until the next reset, which checks it again.
        optimaru::DecomposedObjective<Tasks> calc;
        auto direct = std::make_unique<ConflictCost<Tasks>>();
        ConflictCost<Tasks>* held = direct.get();
        calc.add(std::move(direct));
        Tasks a = assignmentA<Tasks>();
        calc.rebuild(a);
        held->set_offset(1.0);
        held->set_clip(false);
        bool refused = false;
        try {
            calc.rebuild(a);
        } catch (const std::logic_error&) {
            refused = true;
        }
        check(refused, "a later edit is refused at the next reset");
    }
    std::printf("\n");
}
}  // namespace

// --- (r) a negative threshold is computed exactly ---------------------------
// Real-valued raw costs: four workers, tasks with a load in tenths (assignment),
// and the same loads on lists (sequence). No medium level.
namespace {
struct Loads {
    int workerCount = 4;
    std::vector<double> load{0.1, 0.2, 0.7, 0.3, 0.6, 0.4};
    std::vector<int> assignment = std::vector<int>(6, -1);
};
struct LoadRoutes : Routes {
    std::vector<double> load;
};
}  // namespace

template <>
struct optimaru::SolutionTraits<Loads> : TasksDescriptor<Loads> {};
template <>
struct optimaru::SolutionTraits<LoadRoutes> : RoutesDescriptor<LoadRoutes> {};

namespace {

// The summed load of a worker's tasks. cost() counts its calls, which shows
// the dispatch: an instance on the delta path is costed at reset only.
template <typename S>
struct LoadCost : optimaru::UnitConstraint<S> {
    explicit LoadCost(Level level = Level::Soft, const char* name = "loadCost")
        : optimaru::UnitConstraint<S>(name, 1.0, level) {}
    mutable long costCalls = 0;
    double cost(const S& s, int, const std::vector<int>& members) const override {
        ++costCalls;
        double total = 0.0;
        for (int t : members) total += s.load[t];
        return total;
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const S& s, int, const std::vector<int>&, int entity) const override {
        return s.load[entity];
    }
};

// The same cost through a compiled delta group: kGroupedApply, or with
// integral raw deltas the group that keeps a negative threshold.
template <typename S, bool Integral>
struct LoadGroup final : optimaru::UnitConstraintDeltaGroup<S> {
    std::vector<std::size_t> owned;
    void add(std::size_t index, const optimaru::UnitConstraint<S>&) override {
        owned.push_back(index);
    }
    const std::vector<std::size_t>& indexes() const override { return owned; }
    bool has_integral_raw_deltas() const override { return Integral; }
    void on_reset(const S&) override {}
    void evaluate(const S& s, int, const std::vector<int>&, int entity, double* deltas) override {
        for (std::size_t index : owned) deltas[index] = s.load[entity];
    }
};
template <typename S, bool Integral>
struct GroupedLoadCost final : LoadCost<S> {
    explicit GroupedLoadCost(Level level = Level::Soft, double offset = 0.0)
        : LoadCost<S>(level, Integral ? "integralLoadCost" : "groupedLoadCost") {
        this->set_offset(offset);
    }
    const char* delta_group_key() const override { return Integral ? "integralLoad" : "load"; }
    std::unique_ptr<optimaru::UnitConstraintDeltaGroup<S>> create_delta_group() const override {
        return std::make_unique<LoadGroup<S, Integral>>();
    }
};

// A global over the same loads that keeps each worker's sum and adds them up
// in worker order at every cost(): exact, the recipe GlobalConstraint asks
// for (no running sum of real-valued differences).
template <typename S>
struct TotalLoadCost final : optimaru::GlobalConstraint<S> {
    TotalLoadCost() : optimaru::GlobalConstraint<S>("totalLoadCost", 1.0, Level::Soft) {}
    std::vector<double> perWorker;
    static double sumOf(const S& s, const std::vector<int>& members) {
        double total = 0.0;
        for (int t : members) total += s.load[t];
        return total;
    }
    void reset(const S& s, const std::vector<std::vector<int>>& members_per_unit) override {
        perWorker.clear();
        for (const auto& m : members_per_unit) perWorker.push_back(sumOf(s, m));
    }
    void on_unit_changed(const S& s, int unit, const std::vector<int>& members) override {
        perWorker[unit] = sumOf(s, members);
    }
    double cost() const override {
        double total = 0.0;
        for (double v : perWorker) total += v;
        return total;
    }
};

// sequence: the load beyond 0.2 on a route. cost() sums the route; the piecewise
// answer subtracts running sums of the source lists, as prefix sums do, so
// it can round where cost() does not: route [0.1, 0.2] -> moving 0.2 out is
// (0.1 + 0.2) - 0.1 = 0.20000000000000004 by the sums, 0.2 by cost().
template <typename S>
struct ExcessLoadCost final : optimaru::UnitConstraint<S> {
    ExcessLoadCost() : optimaru::UnitConstraint<S>("excessLoadCost", 1.0, Level::Soft) {}
    mutable long piecewiseCalls = 0;
    static double excess(double load) { return load > 0.2 ? load - 0.2 : 0.0; }
    double cost(const S& s, int, const std::vector<int>& members) const override {
        double total = 0.0;
        for (int c : members) total += s.load[c];
        return excess(total);
    }
    bool supports_piecewise_preview() const override { return true; }
    bool piecewise_cost(const S& s, int, const optimaru::ListPiece* pieces, int count,
                       double& out) const override {
        ++piecewiseCalls;
        double total = 0.0;
        for (int i = 0; i < count; ++i) {
            const optimaru::ListPiece& piece = pieces[i];
            if (piece.detached()) {
                total += s.load[piece.begin];
                continue;
            }
            double before = 0.0, upTo = 0.0;  // running sums of the source list
            const std::vector<int>& route = s.routes[piece.list];
            for (int p = 0; p < piece.end; ++p) {
                if (p < piece.begin) before += s.load[route[p]];
                upTo += s.load[route[p]];
            }
            total += upTo - before;
        }
        out = excess(total);
        return true;
    }
};

using LoadCalc = optimaru::DecomposedObjective<Loads>;
using LoadAssembly = optimaru::ConstraintAssembly<Loads>;

bool identical(const Score& a, const Score& b) {
    return a.hard == b.hard && a.soft == b.soft;
}

// A threshold at offset -1 (a fixed charge of 1 per non-zero worker) on one
// component, the rest of the assembly as given.
LoadAssembly negativeThreshold(LoadAssembly assembly, const std::string& name) {
    return std::move(assembly).offset(name, -1.0).clip(name, true);
}

Score freshScore(const std::function<LoadAssembly()>& build, Loads& s) {
    LoadCalc fresh;
    build().into(fresh);
    fresh.rebuild(s);
    return fresh.score();
}

void move(LoadCalc& calc, Loads& s, int task, int worker) {
    calc.before_assign(task);
    s.assignment[task] = worker;
    calc.after_assign(task);
}

template <typename C>
const C& component(const LoadCalc& calc, std::size_t index) {
    return dynamic_cast<const C&>(*calc.unit_constraint_list()[index]);
}

// Worker 0 takes tasks 0 (0.1) and 1 (0.2) and gives them back, one move at a
// time, in a batch, and in a rejected trial: after each step the incremental
// score is bit for bit the fresh one, and the worker back at raw 0
// contributes 0 (an accumulated raw would be ((0.1 + 0.2) - 0.1) - 0.2 =
// 2.8e-17, charged the whole fixed charge). Each assembly has one soft cell
// in play at a time, so the soft running total is exact too.
void exactSequence(const std::string& label, const std::function<LoadAssembly()>& build) {
    Loads s;
    LoadCalc calc;
    build().into(calc);
    calc.rebuild(s);
    bool agree = true;
    auto step = [&](int task, int worker) {
        move(calc, s, task, worker);
        agree &= identical(calc.score(), freshScore(build, s));
    };
    step(0, 0);
    step(1, 0);
    step(0, -1);
    step(1, -1);
    const Score zero{0, 0.0};
    check(agree && identical(calc.score(), zero),
          label + ": one move at a time, incremental == fresh, back at 0 contributes 0");
    bool rawZero = true;
    for (const auto& entry : calc.constraint_costs()) rawZero &= entry.unit_costs[0] == 0.0;
    check(rawZero, label + ": the raw cost of the emptied worker is exactly 0");

    calc.begin_batch();
    move(calc, s, 0, 0);
    move(calc, s, 1, 0);
    calc.end_batch();
    agree = identical(calc.score(), freshScore(build, s));
    calc.begin_batch();
    move(calc, s, 0, -1);
    move(calc, s, 1, -1);
    calc.end_batch();
    check(agree && identical(calc.score(), zero),
          label + ": in a batch (a compound move), back at 0 contributes 0");

    move(calc, s, 0, 0);
    move(calc, s, 1, 0);
    calc.begin_trial();
    calc.begin_batch();
    move(calc, s, 1, 1);
    move(calc, s, 0, 1);
    calc.end_batch();
    agree = identical(calc.score(), freshScore(build, s));
    calc.begin_rollback();
    calc.begin_batch();
    move(calc, s, 0, 0);
    move(calc, s, 1, 0);
    calc.end_batch();
    calc.end_trial(false);
    agree &= identical(calc.score(), freshScore(build, s));
    calc.begin_trial();
    move(calc, s, 0, -1);
    move(calc, s, 1, -1);
    calc.end_trial(true);
    check(agree && identical(calc.score(), zero),
          label + ": a rejected and an accepted trial move, back at 0 contributes 0");
}

// Random moves (single, batched, rejected trials) over all six tasks: the
// hard level of the negative threshold equals the fresh one exactly, soft
// to rounding, and wherever the fresh raw is 0 the incremental raw is 0 too.
// The unshifted control instance stays on the delta path, and its
// accumulated raw does drift off 0 (the premise of the fix).
void exactRandomWalk() {
    auto build = [] {
        LoadAssembly assembly;
        assembly.emplace<LoadCost<Loads>>(Level::Hard, "hardLoadCost");
        assembly.emplace<LoadCost<Loads>>(Level::Soft, "softLoadCost");
        assembly.emplace<LoadCost<Loads>>(Level::Soft, "controlLoadCost");
        assembly.weight("hardLoadCost", 2.0);  // 2 (x + 1): never a rounding tie in tenths
        return negativeThreshold(negativeThreshold(std::move(assembly), "hardLoadCost"),
                                 "softLoadCost");
    };
    Loads s;
    LoadCalc calc;
    build().into(calc);
    calc.rebuild(s);
    std::mt19937_64 rng(29);
    bool hardExact = true, softClose = true, zerosExact = true;
    int controlDrift = 0;
    for (int step = 0; step < 3000; ++step) {
        const int kind = static_cast<int>(rng() % 4);
        const int t1 = static_cast<int>(rng() % 6), t2 = static_cast<int>(rng() % 6);
        const int w1 = static_cast<int>(rng() % 5) - 1, w2 = static_cast<int>(rng() % 5) - 1;
        const int old1 = s.assignment[t1], old2 = s.assignment[t2];
        if (kind == 0) {
            move(calc, s, t1, w1);
        } else {
            if (kind == 3) calc.begin_trial();
            calc.begin_batch();
            move(calc, s, t1, w1);
            move(calc, s, t2, w2);
            calc.end_batch();
            if (kind == 3) {
                calc.score();
                calc.begin_rollback();
                calc.begin_batch();
                move(calc, s, t2, old2);
                move(calc, s, t1, old1);
                calc.end_batch();
                calc.end_trial(false);
            }
        }
        LoadCalc fresh;
        build().into(fresh);
        fresh.rebuild(s);
        const Score a = calc.score(), b = fresh.score();
        hardExact &= a.hard == b.hard;
        softClose &= std::fabs(a.soft - b.soft) < 1e-9;
        const auto mine = calc.constraint_costs(), theirs = fresh.constraint_costs();
        for (std::size_t c = 0; c < mine.size(); ++c) {
            for (std::size_t u = 0; u < theirs[c].unit_costs.size(); ++u) {
                if (theirs[c].unit_costs[u] != 0.0) continue;
                if (mine[c].name == "controlLoadCost") {
                    controlDrift += mine[c].unit_costs[u] != 0.0;
                } else {
                    zerosExact &= mine[c].unit_costs[u] == 0.0;
                }
            }
        }
    }
    check(hardExact && softClose, "random walk: incremental == fresh (hard exactly) over 3000 moves");
    check(zerosExact, "random walk: a raw that is 0 from scratch is exactly 0 incrementally");
    check(controlDrift > 0, "random walk: the unshifted control's accumulated raw drifts off 0 (" +
                                    std::to_string(controlDrift) + " cells), harmlessly");
    check(component<LoadCost<Loads>>(calc, 2).costCalls == 4,
          "the unshifted control stays on the delta path (cost() at reset only)");
}

// (h) A name holding '#' is refused where a component joins an assembly.
void testHashInAssemblyNames() {
    std::printf("== (h) '#' in a name ==\n");
    auto refusal = [](const std::function<void(LoadAssembly&)>& edit) {
        LoadAssembly assembly;
        try {
            edit(assembly);
        } catch (const std::invalid_argument& error) {
            return std::string(error.what());
        }
        return std::string();
    };
    const std::string expected =
            "Constraint 'load#Cost': a name must not contain '#', which keys repeated instances "
            "(\"<name>#<k>\")";
    check(refusal([](LoadAssembly& a) { a.emplace<LoadCost<Loads>>(Level::Soft, "load#Cost"); }) ==
                          expected &&
                  refusal([](LoadAssembly& a) {
                      a.add(std::make_unique<LoadCost<Loads>>(Level::Soft, "load#Cost"));
                  }) == expected &&
                  refusal([](LoadAssembly& a) {
                      a.emplace_instance<LoadCost<Loads>>(Level::Soft, "load#Cost");
                  }) == expected,
          "add(), emplace() and emplaceInstance() refuse a unit constraint named 'load#Cost'");
    struct HashedTotal final : optimaru::GlobalConstraint<Loads> {
        HashedTotal() : optimaru::GlobalConstraint<Loads>("total#", 1.0, Level::Soft) {}
        void reset(const Loads&, const std::vector<std::vector<int>>&) override {}
        void on_unit_changed(const Loads&, int, const std::vector<int>&) override {}
        double cost() const override { return 0.0; }
    };
    check(refusal([](LoadAssembly& a) { a.emplace<HashedTotal>(); }).find("'total#'") !=
                          std::string::npos &&
                  refusal([](LoadAssembly& a) { a.emplace_instance<HashedTotal>(); })
                                  .find("must not contain '#'") != std::string::npos,
          "a global constraint named 'total#' is refused too");
    check(refusal([](LoadAssembly& a) { a.emplace<LoadCost<Loads>>(Level::Soft, "load_2Cost"); })
                  .empty(),
          "any other name is accepted");
    std::printf("\n");
}

void testExactNegativeThreshold() {
    std::printf("== (r) a negative threshold is a fixed charge, computed exactly ==\n");
    check(((0.1 + 0.2) - 0.1) - 0.2 != 0.0,
          "premise: raw + d - d with real-valued deltas leaves a residue");
    check(optimaru::shape_cost(2.8e-17, 1.0, -1.0, true) == 1.0 &&
                  optimaru::shape_cost(0.0, 1.0, -1.0, true) == 0.0,
          "premise: the threshold at offset -1 charges 1 for any non-zero raw, 0 for 0");
    check(optimaru::shaping_jumps_at_zero(-1.0, true) && !optimaru::shaping_jumps_at_zero(-1.0, false) &&
                  !optimaru::shaping_jumps_at_zero(1.0, true) && !optimaru::shaping_jumps_at_zero(0.0, true),
          "shapingJumpsAtZero: only clip=true with offset < 0");

    exactSequence("unit constraint (kVirtualDelta), soft", [] {
        LoadAssembly assembly;
        assembly.emplace<LoadCost<Loads>>();
        return negativeThreshold(std::move(assembly), "loadCost");
    });
    exactSequence("unit constraint (kVirtualDelta), hard", [] {
        LoadAssembly assembly;
        assembly.emplace<LoadCost<Loads>>(Level::Hard);
        return negativeThreshold(std::move(assembly), "loadCost");
    });
    exactSequence("delta-group members (kGroupedApply), soft and hard", [] {
        LoadAssembly assembly;  // a group needs two members
        assembly.emplace<GroupedLoadCost<Loads, false>>(Level::Soft, -1.0);
        assembly.emplace_instance<GroupedLoadCost<Loads, false>>(Level::Hard, -1.0);
        return assembly;
    });
    exactSequence("global constraint, soft", [] {
        LoadAssembly assembly;
        assembly.emplace<TotalLoadCost<Loads>>();
        return negativeThreshold(std::move(assembly), "totalLoadCost");
    });

    {
        // Which instances leave the delta path: only the negative threshold.
        LoadCalc calc;
        LoadAssembly assembly;
        assembly.emplace<LoadCost<Loads>>(Level::Soft, "plain");
        assembly.emplace<LoadCost<Loads>>(Level::Soft, "literalNegative");
        assembly.emplace<LoadCost<Loads>>(Level::Soft, "thresholdPositive");
        assembly.emplace<LoadCost<Loads>>(Level::Soft, "thresholdNegative");
        std::move(assembly)
                .offset("literalNegative", -1.0)
                .clip("literalNegative", false)
                .offset("thresholdPositive", 1.0)
                .offset("thresholdNegative", -1.0)
                .into(calc);
        Loads s;
        calc.rebuild(s);
        for (int t = 0; t < 6; ++t) move(calc, s, t, t % 4);
        auto calls = [&](std::size_t i) { return component<LoadCost<Loads>>(calc, i).costCalls; };
        check(calls(0) == 4 && calls(1) == 4 && calls(2) == 4,
              "offset 0, the literal formula and a positive threshold keep the delta path");
        check(calls(3) == 4 + 6, "the negative threshold re-runs cost() on every touched worker");
    }
    {
        // A group with integral raw deltas accumulates exactly and keeps the
        // group path, negative threshold included.
        auto build = [] {
            LoadAssembly assembly;
            assembly.emplace<GroupedLoadCost<Loads, true>>();
            assembly.emplace_instance<GroupedLoadCost<Loads, true>>();
            return negativeThreshold(std::move(assembly), "integralLoadCost");
        };
        Loads s;
        s.load = {1.0, 2.0, 7.0, 3.0, 6.0, 4.0};
        LoadCalc calc;
        build().into(calc);
        calc.rebuild(s);
        bool agree = true;
        for (int t = 0; t < 6; ++t) {
            move(calc, s, t, t % 3);
            agree &= identical(calc.score(), freshScore(build, s));
        }
        for (int t = 0; t < 6; ++t) {
            move(calc, s, t, -1);
            agree &= identical(calc.score(), freshScore(build, s));
        }
        check(agree && identical(calc.score(), {0, 0.0}) &&
                      component<LoadCost<Loads>>(calc, 0).costCalls == 4,
              "integral group: exact on the group path (cost() at reset only)");
    }
    exactRandomWalk();

    {
        // sequence previews: the negative threshold previews through cost() on the
        // written-out list, never its piecewise answer, so every preview is
        // the applied score (ScoreChecks::Full: a full recompute).
        LoadRoutes probe;
        probe.routes = {{0, 1}};
        probe.load = {0.1, 0.2};
        ExcessLoadCost<LoadRoutes> excess;
        const optimaru::ListPiece moved{0, 1, 2, false};
        double piecewise = -1.0;
        excess.piecewise_cost(probe, 0, &moved, 1, piecewise);
        check(piecewise != 0.0 && excess.cost(probe, 0, {1}) == 0.0,
              "premise: the running-sum answer rounds off 0 where cost() is exactly 0");

        for (const double offset : {-1.0, 0.0}) {
            LoadRoutes s;
            for (int c = 0; c < 12; ++c) {
                s.city.emplace_back(c, c);
                s.demand.push_back(1);
                s.load.push_back(0.1 * (1 + c % 3));
            }
            s.routes = {{0, 1, 2, 3}, {4, 5, 6, 7}, {8, 9}, {10, 11}};
            optimaru::DecomposedObjective<LoadRoutes> calc;
            optimaru::ConstraintAssembly<LoadRoutes> assembly;
            assembly.emplace<ExcessLoadCost<LoadRoutes>>();
            std::move(assembly).offset("excessLoadCost", offset).into(calc);
            optimaru::Editor<LoadRoutes> editor(s, calc,
                                                         optimaru::ScoreChecks::Full);
            editor.score();
            optimaru::testing::PreviewParity parity;
            bool corrupted = false;
            try {
                parity = optimaru::testing::run_preview_parity(editor, calc, 61, 3000);
            } catch (const optimaru::ScoreMismatchError& error) {
                corrupted = true;
                std::printf("    %s\n", error.what());
            }
            const long calls =
                    dynamic_cast<const ExcessLoadCost<LoadRoutes>&>(*calc.unit_constraint_list()[0])
                            .piecewiseCalls;
            if (offset < 0.0) {
                check(!corrupted && parity.all_equal() && parity.checked > 1000 && calls == 0,
                      "sequence, offset -1: every preview equals the applied score, through cost()");
            } else {
                check(!corrupted && parity.all_equal() && calls > 0,
                      "sequence, offset 0: the piecewise answer still serves the previews");
            }
        }
    }
    std::printf("\n");
}
// --- (s) the sign rule ---------------------------------------------------------

// A value per task that is not validated >= 0 (a signed objective, like
// quadratic assignment's cost matrices): the raw cost is the sum of the
// values of the tasks a worker holds (a unit constraint) or of every
// assigned task (a global). raw_cost_can_be_negative() is true when a value is
// negative.
template <typename S>
struct SignedUnitCost final : optimaru::UnitConstraint<S> {
    explicit SignedUnitCost(std::vector<double> taskValues)
        : optimaru::UnitConstraint<S>("signedUnitCost", 1.0, Level::Soft), values(std::move(taskValues)) {}
    std::vector<double> values;
    double cost(const S&, int, const std::vector<int>& members) const override {
        double c = 0;
        for (int t : members) c += values[t];
        return c;
    }
    bool raw_cost_can_be_negative(const S&) const override {
        return std::any_of(values.begin(), values.end(), [](double v) { return v < 0.0; });
    }
};

template <typename S>
struct SignedGlobalCost final : optimaru::GlobalConstraint<S> {
    explicit SignedGlobalCost(std::vector<double> taskValues)
        : optimaru::GlobalConstraint<S>("signedGlobalCost", 1.0, Level::Soft), values(std::move(taskValues)) {}
    std::vector<double> values;
    std::vector<double> perUnit;
    void reset(const S&, const std::vector<std::vector<int>>& members_per_unit) override {
        perUnit.assign(members_per_unit.size(), 0.0);
        for (std::size_t u = 0; u < members_per_unit.size(); ++u) on_unit_changed(S{}, static_cast<int>(u), members_per_unit[u]);
    }
    void on_unit_changed(const S&, int unit, const std::vector<int>& members) override {
        double c = 0;
        for (int t : members) c += values[t];
        perUnit[unit] = c;
    }
    double cost() const override {
        double c = 0;
        for (double v : perUnit) c += v;
        return c;
    }
    bool raw_cost_can_be_negative(const S&) const override {
        return std::any_of(values.begin(), values.end(), [](double v) { return v < 0.0; });
    }
};

// What resetting a calculator over `component` (and a hard conflict) raises,
// "" when it scores.
template <typename S, typename C>
std::string signedRefusal(std::unique_ptr<C> component, S s) {
    optimaru::DecomposedObjective<S> calc;
    calc.add(std::make_unique<ConflictCost<S>>());
    calc.add(std::move(component));
    try {
        calc.rebuild(s);
        calc.score();
    } catch (const optimaru::SignedCostLevelError& error) {
        return error.what();
    }
    return "";
}

void testSignedCostLevel() {
    std::printf("== (s) a raw cost that can be negative stays off the hard score ==\n");
    using optimaru::SignedCostLevelError;
    // Assignment A: worker0 {0,1,2}, worker1 {3,4}, worker3 {5}; 3 hard conflicts.
    const std::vector<double> bonus{-1, -1, -1, 0, 0, 0};    // can be negative: -3 in total
    const std::vector<double> plain{1, 0, 2, 0, 0, 1};       // never negative
    const std::string hardText =
            "Constraint 'signedGlobalCost' can have a negative raw cost on this problem (a bonus) "
            "and cannot be hard";
    check(SignedCostLevelError::message("signed_global_cost", Level::Hard) ==
                  "constraint 'signed_global_cost' can have a negative raw cost on this problem (a bonus) "
                  "and cannot be hard",
          "the Python wording, hard (what the bindings raise as ValueError)");
    check(SignedCostLevelError::message("signed_global_cost", Level::Medium) ==
                  "constraint 'signed_global_cost' can have a negative raw cost on this problem (a bonus) "
                  "and cannot be medium here, where medium counts toward the hard score",
          "the Python wording, a below_hard medium");
    {
        auto global = std::make_unique<SignedGlobalCost<Tasks>>(bonus);
        global->set_level(Level::Hard);  // flippable: the setter cannot know the data
        check(signedRefusal(std::move(global), assignmentA<Tasks>()) == hardText,
              "a hard global over negative data is refused at reset (it would cancel the 3 conflicts)");
        auto unit = std::make_unique<SignedUnitCost<Tasks>>(bonus);
        unit->set_level(Level::Hard);
        check(signedRefusal(std::move(unit), assignmentA<Tasks>()) ==
                      "Constraint 'signedUnitCost' can have a negative raw cost on this problem (a bonus) "
                      "and cannot be hard",
              "a hard unit constraint over negative data is refused at reset");
    }
    {
        auto global = std::make_unique<SignedGlobalCost<HardFoldTasks>>(bonus);
        global->set_level(Level::Medium);
        check(signedRefusal(std::move(global), assignmentA<HardFoldTasks>()) ==
                      "Constraint 'signedGlobalCost' can have a negative raw cost on this problem (a bonus) "
                      "and cannot be medium here, where medium counts toward the hard score",
              "medium below hard is refused too");
        auto into_soft = std::make_unique<SignedGlobalCost<SoftFoldTasks>>(bonus);
        into_soft->set_level(Level::Medium);
        check(signedRefusal(std::move(into_soft), assignmentA<SoftFoldTasks>()).empty(),
              "medium into soft is allowed");
    }
    check(signedRefusal(std::make_unique<SignedGlobalCost<Tasks>>(bonus), assignmentA<Tasks>()).empty(),
          "on soft, a negative total is a bonus as before");
    {
        auto global = std::make_unique<SignedGlobalCost<Tasks>>(plain);
        global->set_level(Level::Hard);
        check(signedRefusal(std::move(global), assignmentA<Tasks>()).empty(),
              "data without a negative value may sit on hard");
    }
    {
        // The default level counts too, and the check runs at every reset.
        optimaru::DecomposedObjective<Tasks> calc;
        auto owned = std::make_unique<SignedGlobalCost<Tasks>>(bonus);
        SignedGlobalCost<Tasks>* held = owned.get();
        calc.add(std::move(owned));
        Tasks s = assignmentA<Tasks>();
        calc.rebuild(s);
        const auto costs = calc.constraint_costs();
        check(costs.size() == 1 && costs[0].signed_cost && costs[0].unit_costs[0] == -3.0,
              "constraintCosts() reports signedCost on negative data (the Python \"signed\")");
        held->set_level(Level::Hard);
        bool refused = false;
        try {
            calc.rebuild(s);
        } catch (const std::logic_error& error) {  // SignedCostLevelError is a logic_error too
            refused = error.what() == hardText;
        }
        check(refused, "an edit to hard after a reset is refused at the next reset");
        held->values = plain;
        calc.rebuild(s);
        check(!calc.constraint_costs()[0].signed_cost && calc.score().hard == -4,
              "the same instance over plain data scores on hard (4 = 1 + 2 + 1)");
    }
    std::printf("\n");
}

// --- (t) reported knobs are the ones the score uses ---------------------------

bool throwsLogicReset(optimaru::DecomposedObjective<Tasks>& calc, Tasks& s) {
    try {
        calc.rebuild(s);
    } catch (const std::logic_error&) {
        return true;
    }
    return false;
}

void testReportedKnobsAreSampledAtReset() {
    std::printf("== (t) constraintTotals() / constraintCosts() report the knobs of the last reset ==\n");
    optimaru::DecomposedObjective<Tasks> calc;
    auto global = std::make_unique<IdleWorkerCost<Tasks>>();
    IdleWorkerCost<Tasks>* idle = global.get();
    idle->set_level(Level::Hard);  // 1 idle worker under assignment A
    auto unit = std::make_unique<ConflictCost<Tasks>>();
    ConflictCost<Tasks>* conflict = unit.get();
    calc.add(std::move(unit));
    calc.add(std::move(global));
    Tasks s = assignmentA<Tasks>();
    calc.rebuild(s);
    const Score before = calc.score();
    auto totalsOf = [&] {
        double hard = 0, soft = 0;
        for (const auto& total : calc.constraint_totals()) (total.is_hard ? hard : soft) += total.cost;
        return std::make_pair(hard, soft);
    };
    check(before.hard == -4 && totalsOf() == std::make_pair(4.0, 0.0), "after reset: 3 + 1 hard, explained");
    // Edits through the pointers a C++ client still holds, with the
    // validating setters, but no reset.
    idle->set_clip(false);
    idle->set_offset(5.0);
    idle->set_weight(3.0);
    conflict->set_level(Level::Soft);
    conflict->set_weight(7.0);
    check(same(calc.score(), before), "the score keeps the knobs of the last reset");
    check(totalsOf() == std::make_pair(4.0, 0.0),
          "constraintTotals() explains that score (no literal 'hard reward' of -5, no re-levelled cache)");
    const auto costs = calc.constraint_costs();
    check(costs.size() == 2 && costs[0].is_hard && costs[0].level == Level::Hard && costs[0].weight == 1.0 &&
                  costs[1].is_hard && costs[1].offset == 0.0 && costs[1].clip && costs[1].weight == 1.0,
          "constraintCosts() reports the sampled weight / level / offset / clip");
    check(throwsLogicReset(calc, s), "the next reset refuses the literal offset on hard");
    idle->set_clip(true);
    calc.rebuild(s);
    const auto after = calc.constraint_costs();
    check(calc.score().hard == 0 && after[0].level == Level::Soft && after[0].weight == 7.0 &&
                  after[1].offset == 5.0 && totalsOf() == std::make_pair(0.0, 21.0),
          "after the reset the edits are in effect: conflicts 3 x 7 soft, 1 idle below the threshold 5");
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
    testLiteralOffsetGuard();
    testHashInAssemblyNames();
    testExactNegativeThreshold();
    testSignedCostLevel();
    testReportedKnobsAreSampledAtReset();
    if (failures == 0) {
        std::printf("All constraint-level tests PASSED.\n");
        return 0;
    }
    std::printf("%d constraint-level test(s) FAILED.\n", failures);
    return 1;
}

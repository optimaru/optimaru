# Employee scheduling: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path. Each step is
complete by itself, so stop at the first one that does what you need. The engine contracts
behind the steps are in [Extending a use case](../../extending.md); the model and its 30
constraints are in the [guide](README.md) and in [python.md](python.md#the-constraints).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level, shape or pick a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add an opt-in constraint | one header, three lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | one Python test case, one line in the C++ walk, the catalog in the tests |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header, the compiled delta group |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | one line of C++, one of Python, the monolithic calculator |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `unusedSkillCost`, for each assigned shift the skills its employee
holds that some shift requires and this one does not. It keeps multi-skilled workers free for the
shifts only they can take. Every snippet below was compiled and run, and every figure measured, on
optimaru 0.4.3; a later change to the solver moves the figures, not the steps. Paths that start with
`cpp/` are under `optimaru/employee_scheduling/`.

## How a schedule is scored

A shift is assigned an employee (`Shift::employeeIndex`, -1 for nobody). The score is kept per
**unit**: one employee, with the shifts assigned to them as its **members**. An unassigned shift
belongs to no unit. A change or a swap moves one or two shifts, so only the employees they leave
and join are scored again.

This use case has more constraints than it scores by default. Eight are the **defaults**
(`es::defaultConstraints()` in `cpp/include/es/constraints.hpp`); the other 22 are **opt-in**:
they sit only in the second assembly of that file, `catalogConstraints()`, and a caller has to
pick them. Python builds every pick through the **constraint factory**, `factory()` in
[`cpp/bindings.cpp`](../../../optimaru/employee_scheduling/cpp/bindings.cpp): one entry per
Python name, which builds the component and reads its parameters.

There are two scoring paths, and they give the same score for the same schedule
(`es_plugin_test` checks it on 300 random assignments, some with unassigned shifts):

| | `es.solve(schedule)` | `es.solve(schedule, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per pick, `cpp/include/es/constraints/` |
| C++ call | `Solver::solve(schedule)` | `Solver::solve(schedule, calculator)` with a `DecomposedCalculator` |
| Constraints | the eight defaults | the ones you list, defaults and opt-in alike, with your knobs and parameters |
| Steps per second | 3.42 million | 2.46 million |
| Parallel starts | `parallel_solver_count` | one start |

The JSON layer, `models.solve_request()`, and so the CP-SAT benchmark, always search on the
assembly of `model_input.constraints`; only `es.solve(schedule)` without `constraints` and the C++
`Solver::solve(schedule)` reach the monolithic calculators.

The speeds are the demo generator at the size of the benchmark's medium instance
(`generate_demo_data(workers=24, days=28)`: 336 shifts, 24 workers), 2 million steps, one thread
of an Apple M4 Max; [step 6](#6-measure) has the script. Steps 1 to 4 work on the assembly.
Step 5 moves a constraint to the monolithic calculators.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs and parameters that differ from the
defaults. An opt-in constraint is picked like any other:

```python
import optimaru.employee_scheduling as es
from optimaru.employee_scheduling.constraints import MaxConsecutiveDaysCost, MinRestCost, UndesiredDayCost, get_constraints
from optimaru.employee_scheduling.demo import generate_demo_data
from optimaru.employee_scheduling.models import to_schedule

schedule = to_schedule(generate_demo_data().model_input)
changed = {"undesired_day_cost", "min_rest_cost", "max_one_shift_per_day_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    UndesiredDayCost(weight=3, per_day=True),                          # heavier, one per shift
    MinRestCost(min_gap_minutes=720, level="soft"),                    # 12 hours, and soft
    MaxConsecutiveDaysCost(value=5),                                   # an opt-in constraint, picked
]                                                                      # max_one_shift_per_day_cost: dropped
es.solve(schedule, time_limit_seconds=10, constraints=picks)          # solved in place and returned
```

To try weights without solving again, score a finished schedule with other picks:

```python
from optimaru.employee_scheduling.constraints import score

raw = es.constraint_costs(schedule, picks)  # raw cost of every constraint, per employee
print(score(raw, picks))                    # the score those picks give this schedule
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`), the scopes (`worker_indices`,
`shift_indices`) and picking a constraint twice are described in
[`constraints.py`](../../../optimaru/employee_scheduling/constraints.py). There is no medium
level: `level="medium"` raises `ValueError`. A JSON request states the same picks in
`model_input.constraints`. Stop here if the constraint you need is one of the 30.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost** per employee,
positive for a violation and 0 for none (a reward returns 0 or less and sets
`reward = true; level_flippable = false;`, as `DesiredDayCost` does). Weight and level are
applied by the engine.

**Opt-in, not weight 0.** Every constraint here declares `kDefaultWeight = 1`: the weight it gets
once picked. A constraint is off by default because it is in `catalogConstraints()` and not in
`defaultConstraints()`; `constraint_info()` then reports `default_enabled=False`. That is the way a
new constraint takes here. A weight-0 entry in `defaultConstraints()` would still need its factory
entry to be picked, and the tests require weight 1 of every row.

There is no base that asks for one function per shift. The family bases in
[`cpp/include/es/constraints/`](../../../optimaru/employee_scheduling/cpp/include/es/constraints/)
ask for one function of a different shape:

| The constraint reads | Base | You write |
| --- | --- | --- |
| the runs of days an employee works | `DayPresenceConstraint` | `runCost(len, touchesEnd)` |
| an employee's minutes per calendar week | `WeeklyMinutesConstraint` | `weekCost(totalMinutes)` |
| anything else about one employee's shifts | `EsUnitConstraint` (`optimaru::UnitConstraint`) | `cost(schedule, unit, members)` |
| something across employees | `EsGlobalConstraint` (`optimaru::GlobalConstraint`) | `reset()`, `on_unit_changed()`, `cost()` |

The example is a sum over one employee's shifts, so it is the third row: one function. Five
edits:

**1. The component**, `cpp/include/es/constraints/unused_skill_cost.hpp`:

```cpp
#ifndef ES_CONSTRAINTS_UNUSED_SKILL_COST_HPP
#define ES_CONSTRAINTS_UNUSED_SKILL_COST_HPP

#include <bitset>

#include "es/constraints/common.hpp"

namespace es {

// Soft, weight 1, opt-in: unusedSkillCost. Raw cost per employee = for each
// of its assigned, non-pinned shifts, the number of skills it holds that some
// shift requires and this shift does not, so 0 = nobody with a spare skill
// covers a shift that does not need it. Keeps multi-skilled workers free for
// the shifts only they can take.
struct UnusedSkillCost final : EsUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit UnusedSkillCost(double weight = kDefaultWeight)
        : EsUnitConstraint("unusedSkillCost", weight, kDefaultLevel) {}
    double cost(const EmployeeSchedule& s, int unit,
                const std::vector<int>& members) const override {
        const std::uint64_t held = s.employees[unit].skillMask;
        std::size_t c = 0;
        for (int i : members)
            if (!s.shifts[i].pinned)
                c += std::bitset<64>(held & ~s.shifts[i].requiredSkillBit).count();
        return static_cast<double>(c);
    }
};

}  // namespace es

#endif  // ES_CONSTRAINTS_UNUSED_SKILL_COST_HPP
```

The name passed to the base, `"unusedSkillCost"`, is the struct name with a lower-case first letter;
Python derives `unused_skill_cost` from it. A constraint with parameters takes them after the
weight, each with a neutral default (so `catalogConstraints()` can build it), and sets `label` to
tell its instances apart (`DayPatternCost`).

The defaults live in these two constants and nowhere else: Python reads them through
`es.constraint_info()`, and the monolithic calculator checks those of the defaults with
`static_assert`s. `kDefaultLevel` is `Hard` or `Soft`. A hard constraint returns whole numbers,
because a hard contribution is rounded per employee.

`skillMask` and `requiredSkillBit` are filled by `ScoreCalculator::prepare()`, which every path
that scores calls; only skills that some shift requires get a bit. Pinned shifts are settled
history and are not judged again, as in `RequiredSkillCost`.

**2. The catalog**, `cpp/include/es/constraints.hpp`: include the header, and add one line at
the end of `catalogConstraints()`:

```cpp
    assembly.emplace<UnusedSkillCost>();
```

**3. The factory entry**, at the end of the table in `factory()` in `cpp/bindings.cpp`:

```cpp
            {"unused_skill_cost", [](const py::dict&) { return unitOf<UnusedSkillCost>(); }},
```

A constraint with parameters reads them there by their Python field names, with the constructor's
defaults as fallbacks (`param<int>(p, "value", 6)`). The table's order is that of
`CONSTRAINT_NAMES` and `catalogConstraints()`. A worker or shift scope needs nothing from the
constraint: the factory wraps a unit pick that has one in `ScopedUnit`.

**4. The Python spec**, `optimaru/employee_scheduling/constraints.py`, after the other opt-in
constraints:

```python
@dataclass(frozen=True)
class UnusedSkillCost(_Scoped):
    """Per assigned shift, the skills the employee holds that some shift requires and this
    one does not. Default: soft, weight 1."""

    name = "unused_skill_cost"
    label_en = "Unused skills"
    label_ko = "쓰지 않는 기술"
    description_en = (
        "For each assigned shift, the skills its employee holds that some shift requires and this shift does not "
        "(pinned shifts are not counted). One unit of cost is one such skill on one shift; 0 means no employee "
        "with a spare skill covers a shift that does not need it."
    )
    description_ko = (
        "배정된 근무마다, 맡은 직원이 가진 기술 가운데 어떤 근무에는 필요하지만 이 근무에는 필요하지 않은 기술의 "
        "수를 더합니다(고정 근무는 세지 않습니다). 근무 하나에서 그런 기술 하나가 비용 1이며, 0이면 남는 기술을 "
        "가진 직원이 그 기술이 필요 없는 근무를 맡은 경우가 없습니다."
    )
```

`_Scoped` gives a unit constraint its `worker_indices` / `shift_indices`; a global constraint
derives from `_EsConstraint`. Parameters are dataclass fields with their limits in `bounds`. The
JSON constraint model (`models.CONSTRAINT_MODELS`) is generated from the spec.

**5. Its place**, in the same file: `UnusedSkillCost` at the end of `SPEC_CLASSES` (the factory
order), `unused_skill_cost = UnusedSkillCost()` with the other instances, and both names in
`__all__`. It stays out of `_DEFAULTS`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=employee_scheduling
uv sync                                    # rebuilds optimaru.employee_scheduling._core
```

```python
from optimaru.employee_scheduling.constraints import UnusedSkillCost

picks = [*get_constraints(), UnusedSkillCost(weight=5)]
for picked in (None, picks):
    config = es.SolverConfig()
    config.step_count_limit = 2_000_000
    config.random_seed = 1
    schedule = to_schedule(generate_demo_data().model_input)
    es.solve(schedule, config=config, constraints=picked)
    print(sum(es.constraint_costs(schedule, picks)["unused_skill_cost"]["costs"]))
```

On the demo instance (84 shifts, 12 workers) a default solve leaves that sum at 119, and the
solve that picks the constraint at 102.

This is correct but not incremental: without `delta_cost()` the engine calls `cost()` on the
whole of each employee a move touches. [Step 4](#4-make-it-cheaper-per-move) changes that.

## 3. Check it

**A case worked out by hand.** The raw costs of every constraint are checked by hand through the
bindings, a positive and a zero case each, in
`tests/employee_scheduling/test_constraint_costs.py`:

```python
def test_unused_skill_cost():
    # Shifts need A (E0), nothing (E0) and B (E1). E0 holds A, B and C, but no shift needs C:
    # B is spare on the first shift (1), A and B on the second (2). E1 holds only B (0).
    shifts = [(0, 9, 8, "L", "A", 0), (1, 9, 8, "L", "", 0), (2, 9, 8, "L", "B", 1)]
    spec = UnusedSkillCost()
    assert raw(roster([es.Employee("E0", {"A", "B", "C"}), es.Employee("E1", {"B"})], shifts), spec) == [3.0, 0.0]
    exact = [(0, 9, 8, "L", "A", 0), (2, 9, 8, "L", "B", 1)]
    assert raw(roster([es.Employee("E0", {"A", "C"}), es.Employee("E1", {"B"})], exact), spec) == [0.0, 0.0]
```

**Incremental against full.** `es_plugin_test` walks 2,000 random moves on an assembly of every
component, `addEveryComponent()` in `tests/employee_scheduling/cpp/es_plugin_test.cpp`, and
compares each score with a full recompute. An opt-in constraint is in no default assembly, so add it
there:

```cpp
    calc.emplace<UnusedSkillCost>();
```

The same list feeds the walk and the 2-second `ScoreChecks::Full` solve of section (h). From
Python, solve with every score checked against a full recompute:

```python
es.solve(to_schedule(generate_demo_data().model_input), time_limit_seconds=1, random_seed=1,
         constraints=picks, full_assert=True)
```

A move that leaves the score stale raises `RuntimeError`, "Score corruption: incremental score
... != full recompute ...". With the `delta_cost()` of step 4 made wrong on purpose (one too
many per shift), both C++ walks, the full-check solve of (h) and this Python solve failed.

**The catalog.** A 31st constraint fails seven checks that count or list it. Update them:

- `es_plugin_test.cpp`, `verifyCatalog()`: `rows.size() == 30` in two places. Add
  `rowIs<UnusedSkillCost>(rows[24], "unusedSkillCost", Level::Soft, false, false)` after
  `workerDayCountInRangeCost`: `catalog_info()` lists the opt-in unit constraints before the opt-in
  globals, so the six globals move to `rows[25]`..`rows[30]`;
- `tests/employee_scheduling/test_constraint_specs.py`: the list `NAMES`. That one list makes
  five tests fail: `test_list_constraints_is_every_name_the_factory_accepts`,
  `test_every_spec_is_named_after_its_class`, `test_constraint_info_reports_the_cpp_defaults`,
  `test_the_factory_specs_rules_and_catalog_name_the_same_constraints` and
  `test_constraint_costs_metadata_matches_constraint_info`.

Then the whole suite:

```sh
cmake -B build -DOPTIMARU_USE_CASES=employee_scheduling -DOPTIMARU_ENGINE_TESTS=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure           # es_demo, es_plugin_test
uv run pytest -q tests/employee_scheduling tests/test_constraints_contract.py
```

**The tables.** The opt-in table in [python.md](python.md#constraint-catalog) must equal
`es.constraint_info()`; add the row. The count of 30 (22 opt-in) is also written in
`constraints.py`, `__init__.py`, `helpers.py`, `_core.pyi`, `constraints/common.hpp` and the
comments of `constraints.hpp`. The notebook `examples/employee-scheduling.ipynb` prints
`es.list_constraints()`.

## 4. Make it cheaper per move

On the assembly a move costs, for every constraint, one evaluation of the employee a shift leaves
and one of the employee it joins. What that is depends on the constraint:

| The constraint is | Use | Per move | Example to copy |
| --- | --- | --- | --- |
| a term per (employee, shift) | a table in `on_reset()`, `delta_cost()`, and a place in the compiled delta group | one table read, inside the group's one pass | [`required_skill_cost.hpp`](../../../optimaru/employee_scheduling/cpp/include/es/constraints/required_skill_cost.hpp), [`desired_shift_cost.hpp`](../../../optimaru/employee_scheduling/cpp/include/es/constraints/desired_shift_cost.hpp) |
| a sum over pairs of one employee's shifts | `delta_cost()` over the other shifts; in the group the pair walk is shared | one pass over the employee's shifts | [`shift_overlap_cost.hpp`](../../../optimaru/employee_scheduling/cpp/include/es/constraints/shift_overlap_cost.hpp), [`min_rest_cost.hpp`](../../../optimaru/employee_scheduling/cpp/include/es/constraints/min_rest_cost.hpp) |
| runs of days or weekly totals of one employee | `DayPresenceConstraint` / `WeeklyMinutesConstraint`, `cost()` alone | the whole employee again | [`max_consecutive_days_cost.hpp`](../../../optimaru/employee_scheduling/cpp/include/es/constraints/max_consecutive_days_cost.hpp), [`max_weekly_minutes_cost.hpp`](../../../optimaru/employee_scheduling/cpp/include/es/constraints/max_weekly_minutes_cost.hpp) |
| a projection several picks share | a `SharedConstraintContext` (`shared_context_key()`, `bind_shared_context()`) | one projection for all of them | [`day_pattern_cost.hpp`](../../../optimaru/employee_scheduling/cpp/include/es/constraints/day_pattern_cost.hpp) |
| anything that spans employees | `EsGlobalConstraint` with `supports_member_change()` and `on_member_changed()` | what your update costs | [`shift_count_imbalance_cost.hpp`](../../../optimaru/employee_scheduling/cpp/include/es/constraints/shift_count_imbalance_cost.hpp), [`shift_coverage_cost.hpp`](../../../optimaru/employee_scheduling/cpp/include/es/constraints/shift_coverage_cost.hpp) |

A unit constraint with `cost()` alone is correct: the engine then recomputes the whole employee on
every change. `delta_cost()` returns what one shift adds given the others, and must equal `cost(with
the shift) - cost(without it)` exactly. `full_assert` and the walks of step 3 check that it does.
Only nine of the 30 constraints have a delta; the run, week and location constraints recompute the
employee.

The example is of the first kind. Two changes, measured one after the other.

**Compute once per solve, and give it a delta.** The facts never change during a solve, so the
term can be a table, and the delta of one shift is one read from it:

```cpp
    // One table per solve: (employee, shift) -> spare skills.
    void on_reset(const EmployeeSchedule& s) override {
        shiftCount_ = static_cast<int>(s.shifts.size());
        spare_.assign(s.employees.size() * s.shifts.size(), 0);
        for (std::size_t e = 0; e < s.employees.size(); ++e) {
            const std::uint64_t held = s.employees[e].skillMask;
            for (std::size_t i = 0; i < s.shifts.size(); ++i) {
                spare_[e * s.shifts.size() + i] = static_cast<std::uint8_t>(
                        std::bitset<64>(held & ~s.shifts[i].requiredSkillBit).count());
            }
        }
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const EmployeeSchedule& s, int unit, const std::vector<int>&,
                      int entity) const override {
        return memberDelta(s, unit, entity);
    }
    double memberDelta(const EmployeeSchedule& s, int unit, int entity) const {
        if (s.shifts[entity].pinned) return 0.0;
        return spare_[static_cast<std::size_t>(unit) * shiftCount_ + entity];
    }

private:
    std::vector<std::uint8_t> spare_;  // (employee, shift) -> spare skills
    int shiftCount_ = 0;
```

`cost()` sums the same row over the members, skipping pinned shifts. With the eight defaults and the
constraint picked, the assembly went from 1.76 to 1.99 million steps per second. `on_reset()` ran
three times per solve, for 200,000 steps as for 2 million. Under `full_assert` it runs at every
check; 20,000 checked steps on the demo instance took about half a second.

**Join the compiled delta group.** This is the fast path of this use case. The eight assignment
constraints (`requiredSkillCost`, `employeeUnavailabilityCost`, `undesiredDayCost`,
`desiredDayCost`, `desiredShiftCost`, `shiftOverlapCost`, `maxOneShiftPerDayCost`, `minRestCost`)
derive from `GroupableAssignmentConstraint` and share one structural key. At reset the calculator
hands the ones present to `EmployeeAssignmentDeltaGroup` in
[`assignment_delta_group.hpp`](../../../optimaru/employee_scheduling/cpp/include/es/constraints/assignment_delta_group.hpp),
which computes all their deltas in one call: table reads for the per-shift terms, one shared walk
over the employee's other shifts for the pair terms. The group is formed only when at least two of
the three pair constraints are present, no member is picked twice, and every member is one it knows.

To join, derive from `GroupableAssignmentConstraint` (include
`es/constraints/assignment_constraint.hpp` instead of `common.hpp`, and pass the name to that
base), and teach the group the new type: include the header, keep a pointer and an index
(`const UnusedSkillCost* unusedSkill_ = nullptr;`, `std::size_t unusedSkillIndex_ = NONE;`),
recognize it in `add()`:

```cpp
        } else if (const auto* typed = dynamic_cast<const UnusedSkillCost*>(&constraint)) {
            if (unusedSkill_ != nullptr) duplicate_ = true;
            unusedSkill_ = typed;
            unusedSkillIndex_ = index;
        } else {
```

and fill its delta in `evaluate()`, next to the other per-shift terms:

```cpp
        if (unusedSkill_ != nullptr)
            deltas[unusedSkillIndex_] = unusedSkill_->memberDelta(schedule, unit, entity);
```

That took the assembly to 2.04 million steps per second, against 2.46 for the eight defaults
alone. The group declares `has_integral_raw_deltas()`, so every member's delta must be a whole
number; a count is.

Do both halves or neither. A `GroupableAssignmentConstraint` the group does not know marks it
unsupported, and then no group is formed at all: `delta_group_count` of the calculator fell from 1
to 0, and the assembly dropped to 1.45 million steps per second, slower than the constraint with no
delta. Picking a member twice switches the group off in the same way (`delta_group_count` 0). A
scoped pick (`ScopedUnit`) stays out of the group and runs on its own `delta_cost()`; the group
still forms for the others.

## 5. Put it on the default path

Do this when the constraint should be on in every solve. Then `es.solve(schedule)` must score it,
and `es.solve(schedule)` scores with the monolithic calculators: 1.4 times the steps per second of
the assembly, and the only path that runs parallel starts. A request that leaves out `constraints` gets
`models.default_constraints()`, which is built from `get_constraints()`, so it picks up the constraint as
well.

`es_plugin_test` compares the default assembly with the monolithic calculators on random
assignments, so it fails from the moment the constraint is a default until both sides agree.

**1. The default set.** Add `assembly.emplace<UnusedSkillCost>();` to `defaultConstraints()`,
before `ShiftCountImbalanceCost` (the default rows list unit constraints before globals), and keep
its line in `catalogConstraints()`: `catalog_info()` checks that both declare it alike and throws if
not. In `constraints.py`, put `unused_skill_cost` in `_DEFAULTS` at the same place. The weight stays
1: the monolithic calculator adds every default at weight 1 in `int64`.

**2. The term**, in `cpp/src/score_calculator.cpp`. Include the header and `<bitset>`, add the
weight and the level to the two `static_assert`s, and add the term to
`ScoreCalculator::shiftSoftDelta()`:

```cpp
std::int64_t ScoreCalculator::shiftSoftDelta(const Employee& emp, const Shift& shift) {
    // unusedSkillCost: the required skills the employee holds that this shift does not need.
    const std::int64_t spare =
            shift.pinned ? 0 : std::bitset<64>(emp.skillMask & ~shift.requiredSkillBit).count();
    return overlapWithDates(shift, emp.desiredDates)       // desiredDayCost (reward)
           - overlapWithDates(shift, emp.undesiredDates)   // undesiredDayCost (penalty)
           - spare;                                        // unusedSkillCost (penalty)
}
```

That is all for a term per (employee, shift). `incremental_calculator.cpp` builds a table of
`shiftHardPenalty()` and `shiftSoftDelta()` per employee and shift in `rebuild()` and reads it
on every move, so the new term costs nothing per step: the default path ran at 3.53 million
steps per second with the ninth constraint, against 3.42 without. With the term left out, the parity
check failed on its first round (monolith `-2881.39697soft`, assembly `-2968.39697soft`); with
it, `es_plugin_test` passes.

The existing per-shift terms of the monolith count pinned shifts, where their components skip
them: a pinned shift on a missing skill scores `-1hard` on `es.solve(schedule)` and 0 on the
default assembly. The parity check uses no pinned shift, so it does not see this. The new term
skips pinned shifts, as its component does.

A hard pair constraint goes into `ScoreCalculator::pairHardPenalty()`, which the incremental
calculator caches per pair of shifts (within a memory budget). A constraint about runs, weeks or
several employees has no place in these tables: it needs its own state in
`incremental_calculator.cpp` (`before_assign()`, `after_assign()`, `score()`), as
`shiftCountImbalanceCost` has (`balSum_`, `balSumSq_`).

**3. What else moves.** A ninth default changes the default set and every default score:

- in `es_plugin_test.cpp`, `verifyCatalog()`: `defaults.size() == 9` with the new row at
  `defaults[7]` (now `enabled`) and `shiftCountImbalanceCost` at `defaults[8]`; the opt-in rows
  move back to `rows[9]`..`rows[30]`, without the new constraint. With these and the term,
  `es_plugin_test` passes again;
- ten Python tests. Five count the eight defaults: in `test_constraint_specs.py`
  `test_get_constraints_defaults_to_the_eight_default_specs`,
  `test_returned_list_is_a_fresh_copy`, `test_constraint_info_reports_the_cpp_defaults`,
  `test_constraint_costs_defaults_to_the_eight_defaults`, and in `test_scheduling.py`
  `test_constraint_totals_explain_the_score`. Five compare with `REFERENCE_SCORE` in
  `tests/employee_scheduling/_reference.py`, which moves from `-602hard/357.34169soft` to
  `-602hard/351.34169soft` (six spare skills in that roster):
  `test_get_constraints_reproduces_the_builtin_score`,
  `test_weight_none_scores_like_the_explicit_default_weights`,
  `test_offset_zero_reproduces_the_plain_scores` (it slices the first eight picks), and in
  `test_scheduling.py` `test_reference_score_is_the_hand_computed_one` and
  `test_default_assembly_reproduces_builtin_score`. `es_demo` passes: in its hand cases the
  worker holds only the skill the shifts need, or none;
- the tables and the "eight" in [README.md](README.md), [python.md](python.md),
  `constraints.py`, `__init__.py`, `helpers.py`, `_core.pyi`, `bindings.cpp`, the comments of
  `constraints.hpp`, `common.hpp` and `score_calculator.cpp`;
- the CP-SAT model the benchmark compares against, `external_solver.employee_scheduling` in
  [external-solver](https://github.com/optimaru/external-solver): one plugin per constraint, which
  subclasses the spec of step 2 and adds `build()`; its defaults come from `constraint_info()`.
  The benchmark instances carry `default_constraints()`, so without that plugin
  [`benchmarks/employee-scheduling`](../../../benchmarks/employee-scheduling) no longer compares
  one objective. An opt-in constraint needs the plugin too, for requests that pick it. Then the
  published results, and `CHANGELOG.md`.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed:

```python
import time

from optimaru.employee_scheduling.constraints import get_constraints

request = generate_demo_data(workers=24, days=28)
steps = 2_000_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    schedule = to_schedule(request.model_input)
    config = es.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    config.parallel_solver_count = 1
    # config.guided_moves = False    # makes both paths take the same steps: a check
    start = time.perf_counter()
    es.solve(schedule, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e6:5.2f} M steps/s  {schedule.score}")
```

Each path repeats itself exactly, but the two do not take the same steps: guided moves pick from
the list of employees with a hard violation, which the monolith and the assembly keep in
different orders. Both ended on `0hard/23040soft` here, but not at 200,000 steps (23039 and
23040). With `config.guided_moves = False` they took the same steps and ended on the same score,
before step 2 and after step 5 alike; if they differ then, the two paths no longer score alike.

| Measured at the medium size, median of three runs | Steps per second |
| --- | --- |
| default path, eight constraints | 3.42 million |
| assembly, eight constraints | 2.46 million |
| assembly, eight + the constraint of step 2 (`cost()` alone) | 1.76 million |
| assembly, eight + the constraint with the table and delta of step 4 | 1.99 million |
| assembly, eight + the constraint as a group member the group does not know | 1.45 million |
| assembly, eight + the constraint in the delta group (step 4) | 2.04 million |
| default path, nine constraints (step 5) | 3.53 million |

The builds ran in turn, one run each per round, while other jobs shared the machine. The runs of
a row differed by up to 3%, so the two default path rows are one figure.

**Quality at a fixed time.** A constraint changes what the search can reach, so compare scores at
the same budget over several seeds, before and after:

```python
for seed in range(1, 6):
    schedule = to_schedule(generate_demo_data(workers=48, days=28).model_input)
    es.solve(schedule, time_limit_seconds=5, random_seed=seed, constraints=picks)
    print(seed, schedule.score)
```

There is no quality bench binary. `./build/employee_scheduling/es_demo 14 5 10` checks the
monolith's hand cases, then solves a generated roster (14 days, 5 shifts a day) for 10 seconds
on the default path. Against CP-SAT:
`uv run employee-scheduling-compare --seconds 10 --workers 8` in
`benchmarks/employee-scheduling/cpsat`
([README](../../../benchmarks/employee-scheduling/README.md)); it solves through `solve_request`,
so on the assembly with each instance's constraints.

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/es/constraints/<subject>_cost.hpp` | new | | table, delta, group base | |
| `cpp/include/es/constraints.hpp` | include, `catalogConstraints()` | | | `defaultConstraints()` |
| `cpp/bindings.cpp` | factory entry | | | counts in the text |
| `constraints.py` | spec, instance, `SPEC_CLASSES`, `__all__` | | | `_DEFAULTS` |
| `cpp/include/es/constraints/assignment_delta_group.hpp` | | | the new member | |
| `tests/employee_scheduling/test_constraint_costs.py` | | hand case | | |
| `tests/employee_scheduling/cpp/es_plugin_test.cpp` | | `addEveryComponent()`, catalog | | catalog |
| `tests/employee_scheduling/test_constraint_specs.py` | | `NAMES` | | default lists, scores |
| `docs/use-cases/employee-scheduling/python.md`, `README.md` | | table row | | defaults |
| `cpp/src/score_calculator.cpp` | | | | the term, `static_assert`s |
| `tests/employee_scheduling/_reference.py`, `test_scheduling.py` | | | | expected scores |
| external-solver, `benchmarks/employee-scheduling` | | | | the same constraint, results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

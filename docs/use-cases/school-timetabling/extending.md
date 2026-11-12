# School timetabling: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its seventeen constraints are in [python.md](python.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-two-functions) | Add a constraint, off by default | one header, one line of C++, one Python class |
| [3](#3-check-it) | Prove it right | one C++ test case, the catalog checks in five test files |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the weight field, the two monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `specialRoomLastPeriodCost`, the special rooms (lab, gym, music room,
...) that lessons take in the last period of a day. It keeps those rooms free at the end of the day
for the clubs that use them after school. Every snippet below was compiled and run on optimaru 0.5.1,
and every figure measured on optimaru 0.4.3; a later change to the solver moves the figures, not the
steps.

**Two domains, one package.** `optimaru.school_timetabling` holds two independent C++
domains. The quickstart domain (`st::`, `domain.hpp`, `solver.hpp`, `score_calculator.cpp`)
is the small model of the original quickstart: lessons on a combined slot-and-room value,
seven fixed terms, no constraint registry, reached with `solve(..., mode="quickstart")`
and benchmarked by track 1. This guide is not about it. Everything below is the
canonical domain (`st::canon`, the `canonical_*` files), which `solve()` uses by default
and which carries the constraint catalog.

## How a timetable is scored

`solve()` takes a `TimetableProblem` dict ([API_SPEC.md](API_SPEC.md)) and converts it
into a `canon.Timetable`: one lesson per weekly period of a course, a parallel group
merged into one lesson, fixed slots as pinned lessons. A lesson is assigned a timeslot.
The score is kept per **unit**: one timeslot, with the lessons assigned to it as its
**members**. A move changes one or two lessons, so only the slots they leave and join
are scored again.

There are two scoring paths:

| | `st.solve(problem)` | `st.solve(problem, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/canonical_score_calculator.cpp` and `cpp/src/canonical_incremental_calculator.cpp` | the assembly: one component per constraint, under `cpp/include/st/constraints/` |
| Constraints | the seventeen, soft weights from `weights=` | the ones you list, with your knobs |
| Steps per second | 1.31 million | 0.32 million |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are the M school of `benchmarks/school-timetabling/gen_canonical.py` (12
classes, 408 lessons, 34 slots), 2 million steps, one thread of an Apple M4 Max;
[step 6](#6-measure) has the script. With `constraints=` Python builds a
`canon.DecomposedCalculator` and hands it to `canon.Solver(config).solve(table, calculator)`.

For the same timetable the two paths give the same score: `st_canonical_plugin_test`
checks the default assembly against the monolith on 500 random assignments and 3,000
random moves, bit for bit. They do not search alike, though. The violation-guided
lesson choice reads the monolith's hot sets on one path and the assembly's penalized
slots on the other, so the same seed and step count end on different timetables (on
the M school after 2 million steps: 1266 on the default path, 1164 on the assembly).
Each path alone repeats exactly. Steps 1 to 4 work on the assembly. Step 5 moves a
constraint into the monolithic calculators.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import sys

sys.path.insert(0, "benchmarks/school-timetabling")
from gen_canonical import generate

import optimaru.school_timetabling as st
from optimaru.school_timetabling.constraints import Assembly, ClassGapCost, TeacherMaxPeriodsCost, get_constraints

problem = generate("S")  # a TimetableProblem dict (API_SPEC.md): 6 classes, 204 lessons
changed = {"class_gap_cost", "teacher_max_periods_cost", "teacher_workday_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    ClassGapCost(weight=40),                                           # heavier (default 10)
    TeacherMaxPeriodsCost(level="soft", weight=50),                    # hard by default
]                                                                      # teacher_workday_cost: dropped
result = st.solve(problem, time_limit_seconds=10, random_seed=1, constraints=picks)
print(result["status"], result["objective_value"], result["penalties"])

# The same picks, written as edits of the defaults:
same = Assembly().add("class_gap_cost", weight=40).add(TeacherMaxPeriodsCost(level="soft", weight=50))
same.drop("teacher_workday_cost")
```

`penalties` in the result are always the raw soft counts of all soft constraints;
`objective_value` weighs them with your picks (a dropped constraint counts 0). To try weights
without solving again, score the finished timetable with other picks:

```python
from optimaru.school_timetabling.constraints import score

converted = st.convert_problem(problem)
st.inject_solution(converted, result["lessons"])            # the result back on a canon.Timetable
raw = st.constraint_costs(converted.table, picks)          # raw cost of every constraint, per slot or total
print(score(raw, picks))                                   # the score those picks give this timetable
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`), picking a constraint twice, and
partial timetabling (`unassigned_lessons_cost` demoted to soft) are described in
[`constraints.py`](../../../optimaru/school_timetabling/constraints.py) and
[python.md](python.md#plugin-constraint-assembly-constraints). On the default path,
`weights={"teacher_gap_cost": 40}` changes a soft weight without leaving the fast
path. Stop here if the constraint you need is one of the seventeen.

## 2. A new constraint from two functions

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are applied
by the engine. This use case has no base that asks for one function. The existing
components sit in families that share state, and a new constraint should not join one by
accident:

| Family | Base | Members | Shared state |
| --- | --- | --- | --- |
| `constraints/slot/` | `GroupableSlotConstraint` | the six per-slot constraints (conflicts, disallowed slots, room capacity, avoided and non-preferred periods) | one `SlotDeltaGroup` evaluates all six per member change |
| `constraints/global/` | `MemberChangeGlobalConstraint` | per course, per block group, unassigned lessons, the two structural constraints | own totals, kept in `on_member_changed()` |
| `constraints/teacher/` | `TeacherDayMaskConstraint` | the five per-(teacher, day) mask constraints | one `TeacherMaskHub` per assembly; each constraint reads one of its metrics |
| `constraints/class_gap_cost.hpp` | `MemberChangeGlobalConstraint` | `classGapCost` | its own class-day masks |

The example reads only the slot and the lessons on it, so it is a plain
`CanonUnitConstraint` with `cost()` and `delta_cost()`. Do not derive it from
`GroupableSlotConstraint`: `SlotDeltaGroup::add()` knows the six by type, marks the
group unsupported for anything else, and the engine then drops the group for all six.
[Step 4](#4-make-it-cheaper-per-move) has the measurement. Four edits:

**1. The component**, `optimaru/school_timetabling/cpp/include/st/constraints/special_room_last_period_cost.hpp`:

```cpp
#ifndef ST_CONSTRAINTS_SPECIAL_ROOM_LAST_PERIOD_COST_HPP
#define ST_CONSTRAINTS_SPECIAL_ROOM_LAST_PERIOD_COST_HPP

#include "st/constraints/common.hpp"

namespace st::canon {

// specialRoomLastPeriodCost — soft, off by default (weight 0). Raw cost per
// slot = the special rooms its lessons need (roomTypeDemand summed; a merged
// lesson counts every member's room) when the slot is the last period of its
// day, so 0 = the special rooms stay free in the last period, for the clubs
// that use them after school.
struct SpecialRoomLastPeriodCost final : CanonUnitConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit SpecialRoomLastPeriodCost(double weight = kDefaultWeight)
        : CanonUnitConstraint("specialRoomLastPeriodCost", weight, kDefaultLevel) {}
    void on_reset(const Timetable& table) override { detail::requirePrepared(table); }
    double cost(const Timetable& table, int unit, const std::vector<int>& members) const override {
        if (!lastOfDay(table, unit)) return 0.0;
        std::int64_t rooms = 0;
        for (int i : members) rooms += roomsOf(table.lessons[i]);
        return static_cast<double>(rooms);
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const Timetable& table, int unit, const std::vector<int>& /*members*/,
                      int entity) const override {
        return lastOfDay(table, unit) ? static_cast<double>(roomsOf(table.lessons[entity])) : 0.0;
    }

private:
    // No later period of the same day exists (days may differ in length).
    static bool lastOfDay(const Timetable& table, int slot) {
        const int day = table.dayOfSlot[slot];
        for (int period = table.periodOfSlot[slot] + 1; period <= table.maxPeriod; ++period) {
            if (table.slotAt(day, period) >= 0) return false;
        }
        return true;
    }
    static std::int64_t roomsOf(const Lesson& lesson) {
        std::int64_t rooms = 0;
        for (int demand : lesson.roomTypeDemand) rooms += demand;
        return rooms;
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_SPECIAL_ROOM_LAST_PERIOD_COST_HPP
```

`delta_cost()` is what one lesson adds to a slot given the other lessons; it must
equal `cost(with it) - cost(without it)` exactly. The name passed to the base,
`"specialRoomLastPeriodCost"`, is the struct name with a lower-case first letter; Python
derives `special_room_last_period_cost` from it. That one name is the constraint's name in
every layer: the catalog, `constraint_costs()`, the `penalties` keys and the `weights`
keys.

The defaults live in the two constants and nowhere else; Python reads them through
`st.constraint_info()`. `kDefaultLevel` is `Hard` or `Soft`: this use case has no medium
level, and `level="medium"` raises `ValueError`. A hard constraint's weight must be a whole
number, because the monolithic calculators add the hard total in `int64` (a
`static_assert` in both files checks it once the constraint is in them). Every component
requires `ScoreCalculator::prepare(table)` to have run, hence `requirePrepared` in
`on_reset()`.

**2. The assembly**, `cpp/include/st/canonical_constraints.hpp`: include the header, and
add one line to `detail::buildDefaultSet()`, after `NonPreferredPeriodCost`:

```cpp
    add(std::make_unique<SpecialRoomLastPeriodCost>());
```

`defaultConstraints()` and `constraintOrder()` are both built from that list, and its
order is the catalog order of `list_constraints()`: the CP-SAT reference's numbering,
then the two structural constraints last. A constraint has to be in it for Python to see it.

**3. The Python spec**, `optimaru/school_timetabling/constraints.py`:

```python
class SpecialRoomLastPeriodCost(Constraint):
    """Special rooms taken in the last period of a day. Opt-in: soft, weight 0."""

    name = "special_room_last_period_cost"
    label_en = "Special rooms in the last period"
    label_ko = "마지막 교시 특별실 사용"
    description_en = (
        "Special rooms needed by lessons in the last period of their day (a parallel group counts each "
        "section's room). 0 means every special room is free in the last period, for after-school use."
    )
    description_ko = (
        "그날 마지막 교시에 놓인 수업이 쓰는 특별실 수입니다(분반 묶음은 분반마다 셉니다). "
        "0이면 마지막 교시에 모든 특별실이 비어 방과 후에 쓸 수 있습니다."
    )
```

**4. Its default instance**, in the same file: `special_room_last_period_cost =
SpecialRoomLastPeriodCost()` with the other instances, that instance in `_DEFAULTS` at
the same place as in `buildDefaultSet()`, and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=school_timetabling
uv sync                                    # rebuilds optimaru.school_timetabling._core
```

```python
from optimaru.school_timetabling import canon
from optimaru.school_timetabling.constraints import SpecialRoomLastPeriodCost

problem = generate("M")
others = [spec for spec in get_constraints() if spec.name != "special_room_last_period_cost"]
picks = [*others, SpecialRoomLastPeriodCost(weight=20)]
config = canon.SolverConfig()
config.step_count_limit = 2_000_000
config.random_seed = 1
result = st.solve(problem, config=config, constraints=picks)
print(result["penalties"]["special_room_last_period_cost"])
```

On the M school that count is 9. A default solve of the same length
(`constraints=None`) leaves it at 22. The new key is in every result's `penalties`
from this step on, which is a change of the response.

The fast path's soft weights, `canon.ObjectiveWeights`, have no field for the constraint
yet; it gets one in [step 5](#5-put-it-on-the-default-path), with its terms in the monolithic
calculators. Until then the default path does not score it: its `objective_value` weighs the
count at the component default (0 here), and `weights=` refuses any other value for it
(`ValueError: ['special_room_last_period_cost'] not on the default path`). A solve with
`constraints=` scores it at the weight you pick, as above.

## 3. Check it

**A case worked out by hand**, in `tests/school_timetabling/cpp/st_canonical_plugin_test.cpp`,
next to the other cases of `verifyRawCostsByHand()` (one positive and one zero case per
constraint, on two days of five periods, read through `constraint_costs()`):

```cpp
    // Two lab lessons and a gym lesson on Tue 5, the last period -> 3 rooms;
    // the same lessons on Tue 4 -> 0.
    checkRaw(
            "specialRoomLastPeriodCost",
            [](Timetable& t) {
                t.roomCapacityByType = {2, 1};
                for (int c = 0; c < 3; ++c) {
                    const int i = addLesson(t, c, c, {c}, slotOf(1, 5));
                    t.lessons[i].roomTypeDemand = {c < 2 ? 1 : 0, c < 2 ? 0 : 1};
                }
                addLesson(t, 3, 3, {3}, slotOf(1, 5));  // no special room
            },
            3.0,
            [](Timetable& t) {
                t.roomCapacityByType = {2, 1};
                for (int c = 0; c < 3; ++c) {
                    const int i = addLesson(t, c, c, {c}, slotOf(1, 4));
                    t.lessons[i].roomTypeDemand = {c < 2 ? 1 : 0, c < 2 ? 0 : 1};
                }
            });
```

```sh
cmake -B build -DOPTIMARU_USE_CASES=school_timetabling && cmake --build build -j
ctest --test-dir build --output-on-failure      # st_test, st_canonical_test, st_canonical_plugin_test
```

**The catalog.** An eighteenth constraint fails these checks. Update them:

- `st_canonical_plugin_test.cpp`, `verifyCatalog()`: the `expected` list of
  `constraintOrder()`, a check of the new row (`rowIs()` expects a constraint that is on by
  default, so check weight 0 and `!default_enabled` directly), and the label "the
  fifteen other constraints flip both ways";
- `test_constraint_specs.py`: `NAMES`, the counts in `test_returned_list_is_a_fresh_copy`
  and `test_assembly_keeps_replace_by_name_and_takes_the_new_knobs`, and the rows of
  `test_constraint_info_reports_the_cpp_defaults`;
- `test_constraints.py`: `DEFAULT_WEIGHTS` (the new name at 0) and the counts in
  `test_list_constraints_matches_core_registry` and `test_drop_component`;
- `test_solve.py` (`test_demo_problem_solves_feasible`, `test_solve_weights_override`)
  and `test_convert.py` (`test_round_trip_rooms_and_penalties`): the soft names and
  weight dicts. All four Python files are in `tests/school_timetabling/`.

```sh
uv run pytest -q tests/school_timetabling tests/test_constraints_contract.py
```

**Incremental against full.** Solve with every score checked against a full recompute.
It raises `RuntimeError: Score corruption: incremental score ... != full recompute ...`
(`ScoreMismatchError` in C++) if a move leaves the score stale:

```python
st.solve(problem, time_limit_seconds=5, random_seed=1, constraints=picks, full_assert=True)
```

Run it with the constraint picked at a weight above 0. The parity checks of
`st_canonical_plugin_test` compare the default assembly with the monolith, where the
constraint sits at weight 0 and scores nothing, so they do not test it. With `delta_cost()`
returning 0 on purpose, the call above raised it.

**The tables and the CP-SAT model.** The constraint table in [python.md](python.md) and
the file list of "Adding a hard or soft cost" in [README.md](README.md#extending) name
every constraint; add the row. The CP-SAT model this use case is compared with lives in this
repository, not in external-solver: `benchmarks/school-timetabling/cpsat`, package
`teacher_scheduling`. It has one plugin per constraint, a subclass of the engine's spec, in
`teacher_scheduling/solver/registry.py` (`CONSTRAINT_TYPES` follows the catalog order),
and its `validate.py` computes the penalties that the benchmark rescoring and
`tests/test_engine_parity.py` compare. Add the constraint there so the benchmark stays like
for like.

## 4. Make it cheaper per move

On the assembly a move costs, for every constraint, one call for the slot a lesson leaves and
one for the slot it joins, unless the constraint belongs to a compiled group. What that call
is depends on the constraint:

| The constraint is | Use | Per move | Example to copy |
| --- | --- | --- | --- |
| a sum of per-lesson terms on one slot | `CanonUnitConstraint` with `delta_cost()` | one `delta_cost` call | step 2 |
| one of the six per-slot constraints | `GroupableSlotConstraint`, a branch in `SlotDeltaGroup` | one `evaluate()` for all six | [`slot/slot_delta_group.hpp`](../../../optimaru/school_timetabling/cpp/include/st/constraints/slot/slot_delta_group.hpp) |
| per course, per block group, per class and day | `MemberChangeGlobalConstraint`: own totals in `on_member_changed()` | what your update costs | [`global/course_max_periods_per_day_cost.hpp`](../../../optimaru/school_timetabling/cpp/include/st/constraints/global/course_max_periods_per_day_cost.hpp) |
| a metric of a teacher's busy periods in a day | `TeacherDayMaskConstraint` and a new `TeacherMaskHub::Metric` | one fold per mask change, shared by the family | [`teacher/teacher_gap_cost.hpp`](../../../optimaru/school_timetabling/cpp/include/st/constraints/teacher/teacher_gap_cost.hpp) |

A unit constraint with `cost()` alone is correct: the engine then recomputes the whole
slot on every change. `full_assert` (step 3) checks that `delta_cost()` matches it.

Two things cut the cost of the example.

**Compute once per solve.** The facts never change during a solve, so the "last
period" test and each lesson's room count can be tables. `on_reset()` runs when the
objective is rebuilt: once at the start of a solve on this assignment-shaped use case,
and on every step under `full_assert`:

```cpp
    // Two tables per solve: cost() and delta_cost() below are then array reads.
    void on_reset(const Timetable& table) override {
        detail::requirePrepared(table);
        last_.assign(table.slotCount(), 0);
        lastSlots_.clear();
        for (int slot = 0; slot < table.slotCount(); ++slot) {
            if (!lastOfDay(table, slot)) continue;
            last_[slot] = 1;
            lastSlots_.push_back(slot);
        }
        rooms_.resize(table.lessons.size());
        for (std::size_t i = 0; i < table.lessons.size(); ++i)
            rooms_[i] = roomsOf(table.lessons[i]);
    }
    // The engine asks this constraint about the last periods only.
    const std::vector<int>* scoped_units() const override { return &lastSlots_; }
    double delta_cost(const Timetable& /*table*/, int unit, const std::vector<int>& /*members*/,
                      int entity) const override {
        return last_[unit] != 0 ? static_cast<double>(rooms_[entity]) : 0.0;
    }
```

`cost()` reads the same two tables, and the members `std::vector<char> last_`,
`std::vector<int> lastSlots_` and `std::vector<std::int64_t> rooms_` follow the two
helpers of step 2.

**Say which units the constraint can charge.** `scoped_units()` returns the slots the constraint is
ever non-zero for (here the five last periods of 34), and the engine skips it on all
others. The skip is exact only if the constraint is 0 outside the list. The list is also the
set of slots the constraint evaluates, so a pick with `clip=False` and an offset charges only
those slots.

With the constraint at weight 20 the assembly went from 0.376 to 0.388 million steps per
second, and the solve ended on the same objective (1401), as it must. The gain is small
because the step-2 loop was short. Two other routes were measured and lost:

| Assembly on the M school, 2 million steps | default constraints | + the constraint at weight 20 |
| --- | --- | --- |
| step 4: table and scope | 0.319 million | 0.388 million |
| the constraint as a seventh member of `SlotDeltaGroup`, no scope | 0.302 million | 0.361 million |
| the constraint derived from `GroupableSlotConstraint` without a group branch | 0.233 million | 0.279 million |

The last row is the trap of step 2: `delta_group_count` drops from 1 to 0 and the six
per-slot constraints fall back to one call each. `canon.default_calculator()` shows the count
after `score(table)`.

**Keep raw costs whole numbers.** Every constraint here counts lessons, rooms or periods, so
sums stay exact and a hard total is never rounded.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`st.solve(problem)` must score it, and that path scores with the monolithic calculators:
four times the steps per second of the assembly, and the only path that runs parallel
starts. `st_canonical_plugin_test` compares the default assembly with the monolithic
calculators (checks (a), (b), (e) and (g)), so it fails from the moment the weight is
not 0 until both sides agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 5.0;`. A
soft weight may be fractional; a hard one must be whole.

**2. The fast path's weight field.** `st.solve(problem, weights=...)` sets the soft weights of
the monolithic calculators through `ObjectiveWeights` (`cpp/include/st/objective_weights.hpp`).
Add the field, which reads the default from the component:

```cpp
    double specialRoomLastPeriodCost = SpecialRoomLastPeriodCost::kDefaultWeight;
```

and, in `cpp/bindings.cpp`, the field in `kWeightFields`, an argument of the
`ObjectiveWeights` constructor, and the field in its `__eq__` and `__repr__`; the stub
`_core.pyi` lists the same field. Python finds the field by the constraint's name, so
`weights={"special_room_last_period_cost": 8}` works from here on. A hard constraint needs
no field.

**3. The full recompute**, `canonical_score_calculator.hpp` and `.cpp`: a `Breakdown`
member named like the constraint, its term at the end of `Breakdown::softPenalty()`, and the
count in the room-demand loop of `computeBreakdown()`. Both calculators need the "last
period" table, so it became a helper, `ScoreCalculator::lastPeriodSlots(table)`, the
loop of step 2 over every slot.

**4. The incremental calculator**, `canonical_incremental_calculator.hpp` and `.cpp`: a
running total `lastRooms_` and the table `lastSlot_`, both set in `rebuild()`.
`insert()` and `retract()` already walk the lesson's room demand for
`roomCapacityCost`, so the term is one line in each loop, in `insert()`

```cpp
        if (lastSlot_[slot] != 0) lastRooms_ += demand;
```

and in `retract()`:

```cpp
        if (lastSlot_[slot] != 0) lastRooms_ -= demand;
```

`score()` adds `w.specialRoomLastPeriodCost * static_cast<double>(lastRooms_)` last, in
the same order as `softPenalty()`: the soft total is a sum of doubles, and the parity
checks compare bit for bit. A hard constraint goes into `Breakdown::hardTotal()` and the
`hard` sum of `score()` instead; the hot sets that guide the default path's lesson
choice are optional for it.

With these, `ctest` passed again (after the catalog check of step 3 expects weight 5 and
on by default; `rowIs()` serves now), and a default-path solve with `full_assert=True`
passed. The default path ran at 1.34 million steps per second with the eighteenth constraint
in it, and on the M school it left 15 special rooms in the last period instead of 22.

**5. What else moves.** A constraint that is on by default changes every default score. Up
to ten Python tests failed, all for one reason: they weigh `penalties` with a hand-written dict
of default weights that still holds 0 for the constraint (`DEFAULT_WEIGHTS` in
`test_constraints.py`, the dicts in `test_solve.py`), or expect the opt-in catalog row. The
two `unassigned_lessons` tests below solve against a time limit and fail only when their
timetable keeps a special room in a last period, so a run shows eight or ten:

- `test_constraint_specs.py::test_constraint_info_reports_the_cpp_defaults`;
- `test_constraints.py`: `test_plugin_solve_default_assembly_parity`,
  `test_weight_override_applies_to_score_and_result`, `test_drop_component`,
  `test_relaxed_unassigned_lessons_return_a_partial_timetable`,
  `test_a_plain_list_may_pick_a_constraint_twice`,
  `test_one_hard_instance_keeps_unassigned_lessons_hard`,
  `test_soft_weights_need_not_be_integers`;
- `test_solve.py`: `test_demo_problem_solves_feasible`, `test_solve_weights_override`.

Then also:

- the "six soft" and "20/3/30/10/5/1" in [python.md](python.md), `constraints.py`,
  `__init__.py`, `_core.pyi`, `objective_weights.hpp` and `canonical_constraints.hpp`;
- the CP-SAT plugin and `validate.py` of step 3, which now count in every comparison;
  without them [`benchmarks/school-timetabling`](../../../benchmarks/school-timetabling)
  no longer compares one objective, nor does `tests/test_engine_parity.py` there, which
  checks that both backends report the same penalties;
- the published results in `benchmarks/school-timetabling/RESULTS.md`, and `CHANGELOG.md`.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed:

```python
import time

from optimaru.school_timetabling import canon

problem = generate("M")
steps = 2_000_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    config = canon.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    result = st.solve(problem, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e6:5.2f} M steps/s  objective {result['objective_value']}")
```

Each line repeats its objective run after run. Unlike other use cases, the two lines do
not end on the same objective (see [how a timetable is scored](#how-a-timetable-is-scored)),
so compare a path with itself: a change that should not alter the search must leave
that path's objective as it was. The rate also changes as the search moves on (on the
default path 100,000 steps ran at 4.6 million per second, 1.6 million steps at 1.0
million), so compare rates at the same step count and objective.
The timing includes the conversion of the problem, a few milliseconds.

| Measured on the M school, median of three | Steps per second |
| --- | --- |
| default path, seventeen constraints | 1.31 million |
| assembly, seventeen constraints | 0.32 million |
| assembly, the constraint of step 2 at weight 20 | 0.376 million |
| assembly, the constraint with the table and scope of step 4 | 0.388 million |
| default path, eighteen constraints (step 5) | 1.34 million |

**Quality at a fixed time.** A constraint changes what the search can reach, so compare scores
at the same budget over several seeds, before and after. The canonical domain is
benchmark track 2, which solves the S, M and L schools of `gen_canonical.py` with the
engine and with the CP-SAT model and rescores both with `validate.py`:

```sh
cd benchmarks/school-timetabling
uv run run_bench.py --track2 --sizes S,M,L --seconds 10     # out/RESULTS_track2.md
```

The C++ quality bench `benchmarks/school-timetabling/cpp/st_quality_bench.cpp` and
`run_bench.py` without a flag are track 1: they run the quickstart domain and say
nothing about a canonical constraint. Partial timetabling is track 3 (`--track3`); see the
[benchmark README](../../../benchmarks/school-timetabling/README.md).

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/st/constraints/<subject>_cost.hpp` | new | | table, scope | weight |
| `cpp/include/st/canonical_constraints.hpp` | include, `add` | | | |
| `cpp/include/st/objective_weights.hpp`, `cpp/bindings.cpp`, `_core.pyi` | | | | weight field |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | counts in the text |
| `tests/school_timetabling/cpp/st_canonical_plugin_test.cpp` | | case, catalog | | catalog row |
| `tests/school_timetabling/test_*.py` (four files) | | names, counts, weight dicts | | expected scores |
| `docs/use-cases/school-timetabling/python.md`, `README.md` | | table row | | defaults |
| `cpp/src/canonical_score_calculator.cpp` (+ `.hpp`) | | | | the term, the helper |
| `cpp/src/canonical_incremental_calculator.cpp` (+ `.hpp`) | | | | the term |
| `benchmarks/school-timetabling/cpsat` | | the plugin, `validate.py` | | results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

# Meeting scheduling: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its twelve constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | two C++ test cases, the catalog check and one count in the C++ test, six Python tests |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the two monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `lateStartCost`, for every meeting the time grains between the first
grain of the day it starts on and its starting grain. It fills the mornings first and keeps the
afternoons free. (The existing `meetingEndTimeCost` fills the first days of the calendar instead.)
Every snippet below was compiled and run, and every figure measured, on optimaru 0.4.3; a later
change to the solver moves the figures, not the steps.

## How a schedule is scored

A meeting is assigned one value, a (time grain, room) cell:
`grain_room = time_grain_pos * room_count + room_index`. The score is kept per
**unit**: one cell, with the meeting assignments that start there as its **members**.
A move changes one or two meetings, so only the cells they leave and join are scored
again. Five of the twelve constraints judge a meeting against its own cell and are unit
constraints. The other seven compare meetings in different cells and are **global**:
each keeps one total for the whole schedule.

There are two scoring paths, and they give the same score for the same schedule.
Both are checked against the full recompute, `ScoreCalculator::score()`: the default
assembly on 400 random schedules (`ms_plugin_test`), the incremental calculator over
3,000 random moves (`ms_test`).

| | `ms.solve(schedule)` | `ms.solve(schedule, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/ms/constraints/` |
| Constraints | the twelve, at their defaults | the ones you list, with your knobs |
| Steps per second | 2.43 million | 0.86 million |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are the demo instance (24 meetings, 480 cells), 2 million steps, one thread
of an Apple M4 Max; [step 6](#6-measure) has the script. Steps 1 to 4 work on the
assembly. Step 5 moves a constraint to the monolithic calculators.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import optimaru.meeting_scheduling as ms
from optimaru.meeting_scheduling.constraints import MeetingOverlapCost, RoomChangeCost, get_constraints

schedule = ms.generate_demo_data()
changed = {"meeting_overlap_cost", "room_change_cost", "larger_room_capacity_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    MeetingOverlapCost(weight=40),                                      # heavier (default 10)
    RoomChangeCost(level="medium"),                                     # soft by default
]                                                                       # larger_room_capacity_cost: dropped
ms.solve(schedule, time_limit_seconds=10, constraints=picks)            # solves in place
```

To try weights without solving again, score a finished schedule with other picks:

```python
from optimaru.meeting_scheduling.constraints import score

raw = ms.constraint_costs(schedule, picks)  # raw cost of every constraint: per cell, or one total
print(score(raw, picks))                    # the score those picks give this schedule
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/meeting_scheduling/constraints.py).
Stop here if the constraint you need is one of the twelve.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

[`constraints/common.hpp`](../../../optimaru/meeting_scheduling/cpp/include/ms/constraints/common.hpp)
has three bases. Pick one by what the constraint compares:

| The constraint compares | Base | You write |
| --- | --- | --- |
| a meeting with its own cell (its time, its room) | `PerAssignmentConstraint`, a unit constraint | `assignmentCost(schedule, unit, assignmentIndex)` |
| two meetings, wherever they are | `AssignmentPairConstraint`, a global one | `marginal(schedule, assignmentIndex, unit)`: what this meeting adds against every other placed meeting |
| two meetings that share an attendee | `AttendanceArcConstraint`, a global one | the same `marginal`, over `arcsOf(meeting)` |

The example reads the starting grain, so it is one function. Four edits:

**1. The component**, `optimaru/meeting_scheduling/cpp/include/ms/constraints/late_start_cost.hpp`:

```cpp
#ifndef MS_CONSTRAINTS_LATE_START_COST_HPP
#define MS_CONSTRAINTS_LATE_START_COST_HPP

#include <algorithm>

#include "ms/constraints/common.hpp"

namespace ms {

// Soft, off by default (weight 0): lateStartCost — how late in its day each
// meeting starts, which keeps the afternoons free. Raw cost per meeting = the
// grains between the first grain of the day it starts on and its starting
// grain; 0 = the meeting starts with the first grain of its day.
struct LateStartCost final : PerAssignmentConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit LateStartCost(double weight = kDefaultWeight)
        : PerAssignmentConstraint("lateStartCost", weight, kDefaultLevel) {}
    double assignmentCost(const MeetingSchedule& schedule, int unit,
                          int /*assignmentIndex*/) const override {
        const TimeGrain& start = schedule.timeGrains[schedule.timeGrainPosOf(unit)];
        int firstMinute = start.startingMinuteOfDay;  // of the first grain of that day
        for (const TimeGrain& grain : schedule.timeGrains) {
            if (grain.dayOfYear == start.dayOfYear)
                firstMinute = std::min(firstMinute, grain.startingMinuteOfDay);
        }
        return static_cast<double>((start.startingMinuteOfDay - firstMinute) /
                                   kGrainLengthInMinutes);
    }
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_LATE_START_COST_HPP
```

The name passed to the base, `"lateStartCost"`, is the struct name with a lower-case
first letter. Python derives `late_start_cost` from it.

The defaults live in these two constants and nowhere else; Python reads them through
`ms.constraint_info()`. `kDefaultLevel` is `Hard`, `Medium` or `Soft`, and the weight is
in units of that level. A medium constraint states its weight in medium units (the two
medium constraints here: 1): the domain folds the medium level into the engine's soft score
itself (`kMediumFold` in `domain.hpp`), so no weight carries the 10^6. A hard constraint
returns whole numbers, because a hard contribution is rounded per cell.

**2. The assembly**, `cpp/include/ms/constraints.hpp`: include the header, and add
one line at the end of `defaultConstraints()`:

```cpp
    assembly.emplace<LateStartCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no default solve changes and the monolithic calculators stay as they are.

**3. The Python spec**, `optimaru/meeting_scheduling/constraints.py`:

```python
class LateStartCost(Constraint):
    """How late in its day each meeting starts. Opt-in: soft, weight 0.

    Keeps the afternoons free: a meeting that opens its day costs nothing."""

    name = "late_start_cost"
    label_en = "Late start in the day"
    label_ko = "늦은 시작 시각"
    description_en = (
        "For each meeting, the time grains between the first grain of the day it starts on and its starting "
        "grain, summed. One unit of cost is one grain; 0 means every meeting starts with the first grain of "
        "its day."
    )
    description_ko = (
        "회의마다 시작하는 날의 첫 그레인부터 회의의 시작 그레인까지의 그레인 수를 더합니다. "
        "그레인 1개가 비용 1이며, 0이면 모든 회의가 그날의 첫 그레인에 시작합니다."
    )
```

**4. Its default instance**, in the same file: `late_start_cost = LateStartCost()` with
the other instances, that instance in `_DEFAULTS` after `larger_room_capacity_cost`,
and both names in `__all__`. The catalog lists the unit constraints first and the
global ones after, each group in the order of `defaultConstraints()`, so the new unit
constraint is the sixth row, not the last.

Build and use it:

```sh
export OPTIMARU_USE_CASES=meeting_scheduling
uv sync                                    # rebuilds optimaru.meeting_scheduling._core
```

```python
from optimaru.meeting_scheduling.constraints import LateStartCost

others = [spec for spec in get_constraints() if spec.name != "late_start_cost"]
picks = [*others, LateStartCost(weight=5)]
config = ms.SolverConfig()
config.step_count_limit = 2_000_000
config.random_seed = 1
schedule = ms.generate_demo_data()
ms.solve(schedule, config=config, constraints=picks)
print(sum(ms.constraint_costs(schedule, picks)["late_start_cost"]["costs"]))
```

On the demo instance that sum is 215 grains. A default solve of the same length
(`constraints=None`) leaves it at 333.

What the base gave you: `cost()` of a cell is the sum of `assignmentCost` over the
meetings that start there, and when a meeting moves the engine calls `assignmentCost`
once for the cell it leaves and once for the cell it joins. The constraint is already
incremental.

## 3. Check it

**A case worked out by hand**, in `tests/meeting_scheduling/cpp/ms_plugin_test.cpp`,
next to the other single-component cases in `verifyPartialAssemblies()`:

```cpp
    {  // lateStartCost alone: two days of 8 grains. One meeting starts on the
        // third grain of day 1 (2 grains in), the other opens day 2 (0).
        MeetingSchedule s = makeBase(2, 8, {5, 5});
        addMeeting(s, 2, {}, {}, 2, 0);
        addMeeting(s, 2, {}, {}, 8, 1);
        DecomposedCalculator calculator;
        calculator.emplace<LateStartCost>(1.0);
        checkScore("lateStartCost alone", scoreWith(calculator, s), 0, -2.0);
    }
```

`verifyRawCostsByHand()` in the same file reads every constraint's raw cost through
`constraint_costs()`, a positive and a zero case each; the constraint gets its pair there
too (`checkRaw(..., "lateStartCost", 2.0)` and `0.0` on the same two meetings).

```sh
cmake -B build -DOPTIMARU_USE_CASES=meeting_scheduling && cmake --build build -j
ctest --test-dir build --output-on-failure
```

**The counts.** A thirteenth constraint fails two checks of `ms_plugin_test.cpp` and six
Python tests. Update them:

- `ms_plugin_test.cpp`: `constraint_totals().size() == 12`, and `rows.size() == 12` in
  `verifyCatalog()`. The new row is `rows[5]`, so the seven global rows move up by
  one. `rowIs()` expects a constraint that is on by default, so check the new row's fields
  directly (`!rows[5].default_enabled`);
- `tests/meeting_scheduling/test_constraint_specs.py`: the lists of
  `test_list_constraints_matches_the_spec_names`,
  `test_module_level_instances_keep_every_default` and
  `test_constraint_info_reports_the_cpp_defaults` (the row is
  `("soft", 0.0, True, False, False)`); `unit_names = ms.list_constraints()[:5]` in
  `test_constraint_info_matches_the_raw_cost_metadata` (six unit constraints now); the
  weights written out in `test_weight_none_means_the_component_default`; and
  `test_the_attendance_clash_raw_costs`, which lists every non-zero raw cost of a hand
  schedule: there the constraint reads 2 in cell 5.

**Incremental against full.** Solve with every score checked against a full recompute. It raises
`RuntimeError` ("Score corruption: ...", the engine's `ScoreMismatchError`) if a move leaves the
score stale:

```python
ms.solve(schedule, time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

**The tables.** The constraint tables in [README.md](README.md) and
[python.md](python.md) must equal `ms.constraint_info()`; add the row. The notebook
`examples/meeting-scheduling.ipynb` prints the constraint list.

**The CP-SAT model.** `external_solver.meeting_scheduling` in
[external-solver](https://github.com/optimaru/external-solver) has one plugin per
constraint: a subclass of the Python spec that builds the CP-SAT terms and reads its
defaults from `ms.constraint_info()`. Its test compares its catalog with
`ms.list_constraints()`, so a new constraint needs its plugin there, also while it is off.

## 4. Make it cheaper per move

On the assembly a move costs, for every picked constraint, one call for the cell a meeting
leaves and one for the cell it joins. What that call is depends on the base:

| The constraint is | Use | Per move | Example to copy |
| --- | --- | --- | --- |
| a sum of one term per meeting | `PerAssignmentConstraint` | one `assignmentCost` call | step 2 |
| a sum over pairs of meetings | `AssignmentPairConstraint` | one pass over the other placed meetings | [`meeting_overlap_cost.hpp`](../../../optimaru/meeting_scheduling/cpp/include/ms/constraints/meeting_overlap_cost.hpp) |
| a sum over pairs that share an attendee | `AttendanceArcConstraint` | one pass over the arcs of the moved meeting | [`required_attendance_conflict_cost.hpp`](../../../optimaru/meeting_scheduling/cpp/include/ms/constraints/required_attendance_conflict_cost.hpp) |

The two global bases keep their total from joins and leaves alone
(`optimaru::MemberChangeGlobalConstraint`), so `marginal()` must give the same number
when a meeting leaves a cell as when it joined. `full_assert` (step 3) checks it.

Two more things cut the cost of a constraint that is already incremental.

**Compute once per solve.** The facts never change during a solve, and the term of
step 2 walks all 160 time grains on every call. `on_reset()` runs once, when the
solve starts, so the walk can become a table there. In the header, with `<map>` and
`<vector>` included:

```cpp
    // One table per solve: assignmentCost() below is then an array read.
    void on_reset(const MeetingSchedule& schedule) override {
        PerAssignmentConstraint::on_reset(schedule);
        grainsIntoDay_ = grainsIntoDay(schedule);
    }
    double assignmentCost(const MeetingSchedule& schedule, int unit,
                          int /*assignmentIndex*/) const override {
        return static_cast<double>(grainsIntoDay_[schedule.timeGrainPosOf(unit)]);
    }

    // Per time grain position: the grains between the first grain of its day
    // and it.
    static std::vector<int> grainsIntoDay(const MeetingSchedule& schedule) {
        std::map<int, int> firstMinute;  // day of year -> its earliest starting minute
        for (const TimeGrain& grain : schedule.timeGrains) {
            const auto [day, isNew] =
                    firstMinute.emplace(grain.dayOfYear, grain.startingMinuteOfDay);
            if (!isNew) day->second = std::min(day->second, grain.startingMinuteOfDay);
        }
        std::vector<int> grains;
        grains.reserve(schedule.timeGrains.size());
        for (const TimeGrain& grain : schedule.timeGrains) {
            grains.push_back((grain.startingMinuteOfDay - firstMinute[grain.dayOfYear]) /
                             kGrainLengthInMinutes);
        }
        return grains;
    }

private:
    std::vector<int> grainsIntoDay_;
```

With the constraint at weight 5 the assembly went from 0.61 to 0.81 million steps per
second, and the solve ended on the same score, as it must. Call the base's
`on_reset()`: it refuses a schedule that `ScoreCalculator::prepare()` has not seen.
Under `full_assert` the engine rebuilds after every score, so `on_reset()` runs every
step there.

**A constraint at weight 0 is still computed.** The assembly evaluates every picked unit
constraint on every move, also one whose weight is 0, because `constraint_costs()`
reports its raw cost. `get_constraints()` holds the new constraint at its default weight 0,
so `ms.solve(schedule, constraints=get_constraints())` pays for it: 0.62 million steps
per second with the loop of step 2, 0.82 million with the table, against 0.86 million
for the twelve without it. A caller who does not want the constraint leaves it out of the
picks, as the snippet of step 2 does with `others`.

**Keep raw costs whole numbers.** Every constraint here counts grains, seats or people, so
sums stay exact and a hard total is never rounded. Return `static_cast<double>` of an
integer count.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`ms.solve(schedule)` must score it, and `ms.solve(schedule)` scores with the
monolithic calculators: almost 3 times the steps per second of the assembly, and the only
path that runs parallel starts.

`ms_plugin_test` compares the default assembly with `ScoreCalculator::score()`, and
`ms_test` compares the incremental calculator with it. So from the moment the weight
is not 0 one of the two fails, until all three agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 1.0;`.
It must be a whole number, because the monolithic calculators add in `int64`
(`kSoftWeight<>`, `kMediumWeight<>` and `kHardWeight<>` in `common.hpp` do not compile
otherwise, nor when the constraint is on another level than the one it is summed into).

**2. The full recompute.** A member of `Breakdown` in
`cpp/include/ms/score_calculator.hpp`, and in `cpp/src/score_calculator.cpp` its
weight in `Breakdown::softTotal()` (or `hardTotal()`, `mediumTotal()`) and its term in
the first loop of `computeBreakdown()`, which has the cell's grain position `pos`:

```cpp
    std::int64_t lateStartCost = 0;            // grains into the day at the start
```

```cpp
           kSoftWeight<RoomChangeCost> * roomChangeCost +
           kSoftWeight<LateStartCost> * lateStartCost;
```

```cpp
    const std::vector<int> grainsIntoDay = LateStartCost::grainsIntoDay(schedule);
```

```cpp
        // 13. lateStartCost.
        b.lateStartCost += grainsIntoDay[pos];
```

**3. The incremental calculator.** It keeps one running total per constraint. Declare the
total and the table in `cpp/include/ms/incremental_calculator.hpp`:

```cpp
    std::vector<int> grainsIntoDay_;    // per grain pos: LateStartCost::grainsIntoDay
```

```cpp
    std::int64_t lateStartCost_ = 0;                               // soft 13
```

and in `cpp/src/incremental_calculator.cpp` reset the total and build the table in
`rebuild()`, add the term in `insert()`, subtract it in `retract()`, and weigh the
total in `score()`:

```cpp
    grainsIntoDay_ = LateStartCost::grainsIntoDay(solution);
```

```cpp
    lateStartCost_ += grainsIntoDay_[pos];
```

```cpp
                              kSoftWeight<RoomChangeCost> * roomChangeCost_ +
                              kSoftWeight<LateStartCost> * lateStartCost_;
```

That is all for a per-meeting constraint: the default path ran at 2.38 million steps per
second with the thirteenth constraint in it. Both calculators call the component's own
`grainsIntoDay()`, so the table has one definition.

A constraint over pairs is more work. The pair constraints are kept from occupancy counters per
grain (`roomGrainCnt_`, `personReqCnt_`, `startCnt_` and the others), which
`insert()` and `retract()` read before and after their own marks; a new pair constraint
needs its counters there, and its sum in `computeBreakdown()`.

**4. What else moves.** With the weight at 1 and the three calculators in agreement,
these still failed:

- the check of step 3 on `rows[5]` and the rows of the two Python tests that state
  the weight 0 (`test_constraint_info_reports_the_cpp_defaults`,
  `test_weight_none_means_the_component_default`);
- six checks of `verifyMediumLevel()` in `ms_plugin_test.cpp`: its hand instance has
  30 soft worked out for the twelve constraints, and the constraint adds to it.

Beyond the failures:

- `ms_test.cpp` works out `Breakdown` members and default scores by hand; a default
  constraint gets its case there;
- the tables and the counts ("twelve", "5 hard + 2 medium + 5 soft") in
  [README.md](README.md), [python.md](python.md), `constraints.py`, `__init__.py`,
  `_core.pyi`, `score_calculator.hpp`, `constraints.hpp` and `common.hpp`;
- `printBreakdown()` in `benchmarks/meeting-scheduling/cpp/ms_quality_bench.cpp` lists
  every `Breakdown` member; add the row, or step 6 does not show the constraint;
- the CP-SAT plugin of step 3 now counts in every comparison of
  [`benchmarks/meeting-scheduling`](../../../benchmarks/meeting-scheduling): its
  published results change, and so does `CHANGELOG.md`.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed:

```python
import time

import optimaru.meeting_scheduling as ms
from optimaru.meeting_scheduling.constraints import get_constraints

steps = 2_000_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    schedule = ms.generate_demo_data()
    config = ms.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    ms.solve(schedule, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e6:5.2f} M steps/s  {schedule.score}")
```

Both lines end on the same score: same seed, same steps, same scoring. If they
differ after a change, the two paths no longer score alike.

| Measured on the demo instance, median of three runs | Steps per second |
| --- | --- |
| default path, twelve constraints | 2.43 million |
| assembly, twelve constraints | 0.86 million |
| assembly, twelve + the constraint of step 2 at weight 5 | 0.61 million |
| assembly, twelve + the constraint with the table of step 4 at weight 5 | 0.81 million |
| default path, thirteen constraints (step 5) | 2.38 million |

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
scores at the same budget over several seeds, before and after:

```sh
cmake --build build --target ms_quality_bench
./build/meeting_scheduling/ms_quality_bench scaled:48 1 10     # instance, seed, seconds
```

`ms_quality_bench` runs the default path only, so it measures a constraint after step 5.
It prints the final hard, medium and soft totals, every non-zero `Breakdown` member
and a moves-per-second estimate; `scaled:N` is an instance of N meetings in the demo's
shape, `demo` the demo. Build the tree before and after the change into two build
directories and run the two binaries in turn on the same instances and seeds.
[Solver defaults and quality bench](python.md#solver-defaults-and-quality-bench) shows
what such a comparison reports. Against CP-SAT:
`uv run meeting-scheduling-compare --seconds 10 --workers 8` in
`benchmarks/meeting-scheduling/cpsat`
([README](../../../benchmarks/meeting-scheduling/README.md)).

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/ms/constraints/<subject>_cost.hpp` | new | | table | weight |
| `cpp/include/ms/constraints.hpp` | include, `emplace` | | | |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | counts in the text |
| `tests/meeting_scheduling/cpp/ms_plugin_test.cpp` | | cases, catalog, count | | the row, the medium hand instance |
| `tests/meeting_scheduling/test_constraint_specs.py` | | six tests | | the row, the weights |
| `docs/use-cases/meeting-scheduling/README.md`, `python.md` | | table row | | defaults, counts |
| `cpp/include/ms/score_calculator.hpp`, `cpp/src/score_calculator.cpp` | | | | member, weight, term |
| `cpp/include/ms/incremental_calculator.hpp`, `cpp/src/incremental_calculator.cpp` | | | | total, table, term |
| `tests/meeting_scheduling/cpp/ms_test.cpp` | | | | a case by hand |
| `benchmarks/meeting-scheduling/cpp/ms_quality_bench.cpp` | | | | breakdown row |
| external-solver, `benchmarks/meeting-scheduling` | | the plugin | | results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

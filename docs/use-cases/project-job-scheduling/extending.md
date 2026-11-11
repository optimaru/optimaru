# Project job scheduling: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its four constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | two C++ test cases, six checks that count or list the catalog |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `nonRenewableResourceUsageCost`, what the chosen execution modes take
from the non-renewable resources in total, within capacity or not. It makes the solver prefer modes
that spend less stock. Every snippet below was compiled and run, and every figure measured, on
optimaru 0.4.3; a later change to the solver moves the figures, not the steps. Paths that start with
`cpp/` are under `optimaru/project_job_scheduling/`.

## How a schedule is scored

An allocation (one job) is assigned one number, `modeDelay`: an execution mode of its
job and a delay in days, packed as `delay * maxModeCount + modeSlot`. Start and end
dates are derived: a job starts when its predecessors are done, plus its delay, and
runs for its mode's duration. So one change can shift every job downstream.

The engine keeps a score per **unit**, which here is one value of that number, with
the allocations that hold it as its **members** (1,500 units on the demo: 500 delays
times 3 mode slots). No constraint of this use case is a sum over such units, so all four
are **global** constraints: each has one raw total for the whole schedule, and is told
when any unit changes.

There are two scoring paths, and they give the same score for the same schedule
(`pjs_plugin_test` checks it on 300 random assignments):

| | `pj.solve(schedule)` | `pj.solve(schedule, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/pjs/constraints/` |
| Constraints | the four, at their defaults | the ones you list, with your knobs |
| Steps per second | 4.5 million | 1.6 million |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are the demo instance (24 jobs, 2 projects, 7 resources), 2 million steps,
one thread of an Apple M4 Max; [step 6](#6-measure) has the script. The gap grows with
the instance: on the benchmark's `medium` instance (72 jobs) the two ran at 3.1 and
0.65 million. On a move the monolithic calculators update only the dates that shift,
and every component of the assembly recomputes its total from the whole schedule.
Steps 1 to 4 work on the assembly. Step 5 moves a constraint to the monolithic calculators.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import optimaru.project_job_scheduling as pj
from optimaru.project_job_scheduling.constraints import (
    ProjectDelayCost,
    RenewableResourceCapacityCost,
    get_constraints,
)
from optimaru.project_job_scheduling.demo import generate_demo_data

schedule = generate_demo_data()
changed = {"renewable_resource_capacity_cost", "project_delay_cost", "makespan_cost"}
picks = [
    *[rule for rule in get_constraints() if rule.name not in changed],  # the others as they are
    RenewableResourceCapacityCost(weight=10),                           # heavier (default 1)
    ProjectDelayCost(level="soft"),                                     # medium by default
]                                                                       # makespan_cost: dropped
result = pj.solve(schedule, time_limit_seconds=10, constraints=picks)
```

To try weights without solving again, score a finished schedule with other picks:

```python
from optimaru.project_job_scheduling.constraints import score

raw = pj.constraint_costs(result.to_core(), picks)  # the raw total of every rule
print(score(raw, picks))                            # (hard, medium * 10**9 + soft) under those picks
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/project_job_scheduling/constraints.py).
The score a solve reports keeps whole numbers on every level, so a fractional weight
is rounded there; `score()` gives the exact value. Stop here if the constraint you need is
one of the four.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

All four constraints derive from the one base in
[`constraints/common.hpp`](../../../optimaru/project_job_scheduling/cpp/include/pjs/constraints/common.hpp),
and it asks for one function:

| The constraint reads | Base | You write |
| --- | --- | --- |
| the modes and delays assigned in the whole schedule, and the dates they give | `ScheduleAggregateConstraint` | `evaluate(schedule, startDate, endDate)`, the raw total |

The base hands `evaluate()` the start and end date of every allocation, derived from
the assigned values. Read those, not the `startDate` and `endDate` fields of the
allocations: nothing refreshes the fields while the assembly scores. An unassigned
allocation (`modeDelay == -1`) uses no resource and has delay 0 and duration 0.

The example reads only the modes. Four edits:

**1. The component**, `optimaru/project_job_scheduling/cpp/include/pjs/constraints/non_renewable_resource_usage_cost.hpp`:

```cpp
#ifndef PJS_CONSTRAINTS_NON_RENEWABLE_RESOURCE_USAGE_COST_HPP
#define PJS_CONSTRAINTS_NON_RENEWABLE_RESOURCE_USAGE_COST_HPP

#include "pjs/constraints/common.hpp"

namespace pjs {

// Soft, off by default (weight 0): nonRenewableResourceUsageCost — how much
// of the non-renewable resources the assigned execution modes consume, within
// capacity or not. Raw cost (one global total) = sum over the assigned
// allocations of their mode's requirements on non-renewable resources, in
// resource units, so 0 = no stock is consumed. The capacity rule charges only
// what exceeds a stock; this one prices every unit taken from it.
struct NonRenewableResourceUsageCost final : ScheduleAggregateConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit NonRenewableResourceUsageCost(double weight = kDefaultWeight)
        : ScheduleAggregateConstraint("nonRenewableResourceUsageCost", weight, kDefaultLevel) {}
    double evaluate(const ProjectJobSchedule& schedule, const std::vector<int>& /*startDate*/,
                    const std::vector<int>& /*endDate*/) const override {
        std::int64_t usage = 0;
        for (int a = 0; a < schedule.allocationCount(); ++a) {
            const int value = schedule.allocations[a].modeDelay;
            if (value < 0) continue;  // unassigned: no resource usage
            for (const ResourceRequirement& requirement :
                 schedule.executionModeOf(a, value).resourceRequirements) {
                if (!schedule.resources[requirement.resource].renewable)
                    usage += requirement.requirement;
            }
        }
        return static_cast<double>(usage);
    }
};

}  // namespace pjs

#endif  // PJS_CONSTRAINTS_NON_RENEWABLE_RESOURCE_USAGE_COST_HPP
```

The name passed to the base, `"nonRenewableResourceUsageCost"`, is the struct name with
a lower-case first letter. Python derives `non_renewable_resource_usage_cost` from it.

The defaults live in these two constants and nowhere else; Python reads them through
`pj.constraint_info()`. `kDefaultLevel` is `Hard`, `Medium` or `Soft`, and the weight is
in units of that level. A medium constraint states its weight in medium units
(`projectDelayCost`: 1): the domain folds the medium level into the engine's soft level
itself, as `medium * 10^9 + soft` (`kMediumFold` in `domain.hpp`), so no weight carries
the 10^9 and a soft total has to stay below it. A hard constraint returns whole numbers,
because a hard contribution is rounded.

**2. The assembly**, `cpp/include/pjs/constraints.hpp`: include the header, and add
one line at the end of `defaultConstraints()`:

```cpp
    assembly.emplace<NonRenewableResourceUsageCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no default solve changes and the monolithic calculators stay as they are.

**3. The Python spec**, `optimaru/project_job_scheduling/constraints.py`:

```python
class NonRenewableResourceUsageCost(Constraint):
    """Total use of the non-renewable resources, within capacity or not. Opt-in: soft, weight 0."""

    name = "non_renewable_resource_usage_cost"
    label_en = "Non-renewable resource usage"
    label_ko = "비재생 자원 사용량"
    description_en = (
        "How much of the non-renewable resources (stocks that are used up, such as a budget) the execution "
        "modes chosen for all jobs consume in total, whether or not a capacity is exceeded. One unit of cost "
        "is one resource unit; 0 means no stock is consumed."
    )
    description_ko = (
        "각 작업에 선택된 실행 모드가 비재생 자원(예산처럼 한 번 쓰면 없어지는 자원)을 쓰는 양을, 용량을 "
        "넘는지와 관계없이 모두 더합니다. 자원 1단위가 비용 1이며, 0이면 비재생 자원을 전혀 쓰지 않습니다."
    )
```

**4. Its default instance**, in the same file: `non_renewable_resource_usage_cost =
NonRenewableResourceUsageCost()` with the other instances, that instance at the end of
`_DEFAULTS` (the order of `defaultConstraints()`), and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=project_job_scheduling
uv sync                                    # rebuilds optimaru.project_job_scheduling._core
```

```python
from optimaru.project_job_scheduling.constraints import NonRenewableResourceUsageCost

others = [rule for rule in get_constraints() if rule.name != "non_renewable_resource_usage_cost"]
picks = [*others, NonRenewableResourceUsageCost(weight=1)]
config = pj.SolverConfig()
config.step_count_limit = 2_000_000
config.random_seed = 1
result = pj.solve(schedule, config=config, constraints=picks)
print(pj.constraint_costs(result.to_core(), picks)["non_renewable_resource_usage_cost"]["costs"])
```

On the demo instance that total is 17, with a makespan of 10 days. A default solve of
the same length (`constraints=None`) leaves it at 44, with a makespan of 7: at weight 1
the constraint gives three days for 27 units of stock.

What the base gave you: any change marks the constraint stale, and `evaluate()` runs once
per score, over the whole schedule. That is correct from the first build, and it is the
cost that step 4 removes.

## 3. Check it

**A case worked out by hand**, in `tests/project_job_scheduling/cpp/pjs_plugin_test.cpp`,
next to the other single-component cases in `verifyPartialAssemblies()`:

```cpp
    {  // nonRenewableResourceUsageCost alone: two jobs take 7 + 6 from a
        // stock of 100, far within its capacity.
        ProjectJobSchedule s = makeBase(100, /*renewable*/ false, 0);
        addJob(s, JobType::SOURCE, 0, 0, {}, 0);
        addJob(s, JobType::STANDARD, 1, 7, {0}, 0);
        addJob(s, JobType::STANDARD, 1, 6, {0}, 0);
        DecomposedCalculator calculator;
        calculator.emplace<NonRenewableResourceUsageCost>(1.0);
        checkScore("nonRenewableResourceUsageCost alone", scoreWith(calculator, s), 0, 0, -13);
    }
```

Add the zero case too (the same jobs on a renewable resource), and a positive and a
zero raw cost in `verifyRawCostsByHand()`, where every constraint has both.

```sh
cmake -B build -DOPTIMARU_USE_CASES=project_job_scheduling -DOPTIMARU_ENGINE_TESTS=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure           # pjs_test, pjs_plugin_test
uv run pytest -q tests/project_job_scheduling tests/test_constraints_contract.py
```

**The counts.** A fifth constraint fails six checks that count or list the catalog.
Update them:

- `pjs_plugin_test.cpp`: `constraint_totals().size() == 4` in `verifyConstraintTotals()`,
  and `rows.size() == 4` in `verifyCatalog()`. Add the new row there. `rowIs()` expects a
  constraint that is on by default, so check the new row's fields directly
  (`!rows[4].default_enabled`);
- `tests/project_job_scheduling/test_constraint_specs.py`:
  `test_list_constraints_matches_the_spec_names` (the list),
  `test_constraint_info_reports_the_cpp_defaults` (`EXPECTED_INFO`, and its assertion
  that every row is on by default) and `test_the_medium_level_folds_into_soft` (the raw
  costs it lists);
- `tests/project_job_scheduling/test_constraints.py`:
  `test_project_delay_cost_and_makespan_cost` (the raw costs it lists).

**Incremental against full.** Solve with every score checked against a full
recompute. A move that leaves the score stale raises `RuntimeError`, "Score corruption:
incremental score ... != full recompute ..." (the engine's `ScoreMismatchError`):

```python
pj.solve(schedule, time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

While the default weight is 0, this solve is the only check of what the constraint keeps
between moves. `pjs_plugin_test` walks 1,500 random moves on the default assembly, but
a constraint at weight 0 adds 0 to both sides of every comparison: with the update of step 4
made wrong on purpose, both C++ tests still passed and the solve above failed.

**The tables.** The constraint tables in [README.md](README.md) and
[python.md](python.md) must equal `pj.constraint_info()`; add the row. The notebook
`examples/project-job-scheduling.ipynb` prints `pj.list_constraints()`.

## 4. Make it cheaper per move

On the assembly a constraint is told of every change, and what a move costs is what the
constraint does then:

| The constraint is | Use | Per move | Example to copy |
| --- | --- | --- | --- |
| a total that needs the dates (a resource profile, a project end) | `ScheduleAggregateConstraint` | the date chain and `evaluate()`, over the whole schedule | [`renewable_resource_capacity_cost.hpp`](../../../optimaru/project_job_scheduling/cpp/include/pjs/constraints/renewable_resource_capacity_cost.hpp) |
| a total of the assigned modes alone | the same, with `needsDates()` returning false | `evaluate()` over the whole schedule | [`non_renewable_resource_capacity_cost.hpp`](../../../optimaru/project_job_scheduling/cpp/include/pjs/constraints/non_renewable_resource_capacity_cost.hpp) |
| a sum of one term per allocation, each from its own mode and delay | `optimaru::MemberChangeGlobalConstraint`: keep the total in `on_member_changed()` | two updates | below; bed allocation's [`mixed_gender_room_cost.hpp`](../../../optimaru/bed_allocation/cpp/include/ba/constraints/mixed_gender_room_cost.hpp) |

The first two rows are why the assembly falls behind as the instance grows: each of
the four constraints walks every allocation on every score, and three of them derive the
same date chain, each for itself. The example is of the third kind, so it need not.
Two changes, measured with the constraint at weight 1 (million steps per second):

| The constraint as | demo | `medium` |
| --- | --- | --- |
| step 2: `evaluate()` over the whole schedule | 1.35 | 0.51 |
| the same, with `bool needsDates() const override { return false; }` | 1.43 | 0.55 |
| a member-change global, below | 1.50 | 0.62 |
| the four constraints without it, for reference | 1.55 | 0.65 |

**Keep the total from joins and leaves.** The engine tells a member-change global
which allocation left which value and which it joined. The constraint keeps one integer
and a table of what every allocation takes in each of its mode slots. The base becomes
`optimaru::MemberChangeGlobalConstraint<ProjectJobSchedule>`, and `evaluate()` gives way to:

```cpp
    // A unit is one value of the composite variable (delay x mode slot), its
    // members the allocations that hold it. The table has what each allocation
    // consumes in each mode slot; the total starts from the members as they are.
    void reset(const ProjectJobSchedule& schedule,
               const std::vector<std::vector<int>>& members_per_unit) override {
        detail::requirePrepared(schedule);
        slotCount_ = schedule.maxModeCount;
        usage_.assign(static_cast<std::size_t>(schedule.allocationCount()) * slotCount_, 0);
        for (int a = 0; a < schedule.allocationCount(); ++a) {
            for (int slot = 0; slot < slotCount_; ++slot) {
                for (const ResourceRequirement& requirement :
                     schedule.executionModeOf(a, slot).resourceRequirements) {
                    if (!schedule.resources[requirement.resource].renewable)
                        usage_[cell(a, slot)] += requirement.requirement;
                }
            }
        }
        total_ = 0;
        for (std::size_t unit = 0; unit < members_per_unit.size(); ++unit) {
            for (int a : members_per_unit[unit]) total_ += usage_[cell(a, static_cast<int>(unit))];
        }
    }
    void on_member_changed(const ProjectJobSchedule& /*schedule*/, int unit,
                           const std::vector<int>& /*members*/, int entity, bool joined) override {
        total_ += joined ? usage_[cell(entity, unit)] : -usage_[cell(entity, unit)];
    }
    double cost() const override { return static_cast<double>(total_); }

private:
    // (allocation, value) -> the table cell of the value's mode slot.
    std::size_t cell(int allocation, int value) const {
        return static_cast<std::size_t>(allocation) * slotCount_ + value % slotCount_;
    }

    int slotCount_ = 1;
    std::vector<std::int64_t> usage_;  // allocations x mode slots
    std::int64_t total_ = 0;
```

Every join must be undone exactly by the matching leave, because the engine rolls a
rejected move back through the same calls. An integer total does that; `full_assert`
(step 3) checks it. `reset()` ran twice in a solve, at the start and for the final
score, whether the solve took 200,000 steps or 2 million, so the table costs nothing
per move. Under `full_assert` it runs at every check.

The constraint stays a global although its total is a sum over units. As a unit constraint
it would have one raw cost per value, and `exponent` and `offset` would shape each of
those instead of the total; every constraint of this use case reports one total.

**Keep raw costs whole numbers.** Every constraint here counts days or resource units, so
return `static_cast<double>` of an integer count.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`pj.solve(schedule)` must score it, and `pj.solve(schedule)` scores with the monolithic
calculators: 2.9 times the steps per second of the assembly on the demo, 4.8 times on
`medium`, and the only path that runs parallel starts.

`pjs_plugin_test` compares the default assembly with the monolithic calculators on
random assignments, so it fails from the moment the weight is not 0 until both sides
agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 1.0;`.
It must be a whole number on every level, because the monolithic calculators add in
`int64`.

**2. The term**, in `cpp/src/score_calculator.cpp`. Include the header, read the weight
off the component, add it and the level to the two `static_assert`s, sum the term in
the loop of `ScoreCalculator::score()` that fills `nonRenewableUsage`, and add it to
the soft level:

```cpp
constexpr auto kNonRenewableUsageWeight =
        static_cast<std::int64_t>(NonRenewableResourceUsageCost::kDefaultWeight);
```

```cpp
                nonRenewableUsage[req.resource] += req.requirement;
                nonRenewableTotal += req.requirement;
```

```cpp
                  -(kMakespanWeight * makespan + kNonRenewableUsageWeight * nonRenewableTotal)};
```

**3. The same in `cpp/src/incremental_calculator.cpp`**, which has its own copy of the
weight constants. It keeps the usage per resource in `applyNonRenewable()`; one line
there keeps the total as well (a member declared in `incremental_calculator.hpp` and
set to 0 in `rebuild()`), and `score()` adds it to the soft level:

```cpp
        nonRenewableTotal_ += sign * req.requirement;
```

That is all for a constraint that reads only modes. The default path ran at 4.42 million
steps per second with the fifth constraint in it, against 4.46 without (`medium`: 3.15
against 3.11): no difference. A constraint that reads dates has to follow the propagation in
`after_assign()`: `applyRenewable()` and `setRecordedDates()` are where the totals that
depend on dates are kept.

**4. What else moves.** A constraint that is on by default changes every default score:

- the catalog row of step 3 in `pjs_plugin_test.cpp`, now
  `rowIs<NonRenewableResourceUsageCost>(rows[4], "nonRenewableResourceUsageCost", Level::Soft, 1.0)`,
  and the same row in `test_constraint_info_reports_the_cpp_defaults`;
- the hand-computed scores of every schedule that takes from a stock. With the example
  at weight 1: in `pjs_plugin_test.cpp`, "the hand schedule at the default weights"
  (soft -2 becomes -16) and eight checks of `verifyMediumLevel()` on the same schedule;
  in `tests/project_job_scheduling/cpp/pjs_test.cpp`, the two
  `nonRenewableResourceCapacityCost` cases (soft 0 becomes -13); in `test_constraints.py`,
  `test_non_renewable_resource_capacity_cost`. The parity checks pass;
- the tables and the "four" in [README.md](README.md), [python.md](python.md),
  `constraints.py`, `__init__.py`, `_core.pyi`, `common.hpp` and `bindings.cpp`, and the
  list of constraints in `score_calculator.hpp`;
- the CP-SAT model the benchmark compares against, `external_solver.project_job_scheduling`
  in [external-solver](https://github.com/optimaru/external-solver): one plugin per constraint,
  which subclasses the spec of step 2. Without it,
  [`benchmarks/project-job-scheduling`](../../../benchmarks/project-job-scheduling)
  no longer compares one objective. Then its published results, and `CHANGELOG.md`.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed:

```python
import time

import optimaru.project_job_scheduling as pj
from optimaru.project_job_scheduling.constraints import get_constraints
from optimaru.project_job_scheduling.demo import generate_demo_data

schedule = generate_demo_data()
steps = 2_000_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    config = pj.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    result = pj.solve(schedule, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e6:5.2f} M steps/s  medium {result.score_medium} soft {result.score_soft}")
```

Both lines end on the same score: same seed, same steps, same scoring. If they differ
after a change, the two paths no longer score alike. A step is what `step_count_limit`
counts; the moves that polish a ruin-and-recreate kick are not counted, on either path.

| Measured, median of three runs (million steps per second) | demo | `medium` |
| --- | --- | --- |
| default path, four constraints | 4.46 | 3.11 |
| assembly, four constraints | 1.55 | 0.65 |
| assembly, four + the constraint of step 2 at weight 1 | 1.35 | 0.51 |
| assembly, four + the constraint of step 4 | 1.50 | 0.62 |
| default path, five constraints (step 5) | 4.42 | 3.15 |
| assembly, five constraints (step 5) | 1.53 | 0.63 |

`medium` is the instance of that name in the CP-SAT comparison (72 jobs, 4 projects,
13 resources), built by `instances()` in
`benchmarks/project-job-scheduling/cpsat/project_job_scheduling_cpsat/benchmark.py`.
Two runs of one build differed by up to 3%.

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
scores at the same budget over several seeds, before and after:

```python
for seed in range(1, 6):
    result = pj.solve(schedule, time_limit_seconds=5, random_seed=seed, constraints=picks)
    print(seed, result.score_hard, result.score_medium, result.score_soft)
```

This use case has no quality bench binary. `./build/project_job_scheduling/pjs_demo 10`
solves the C++ demo instance for 10 seconds and prints every new best. Against CP-SAT:
`uv run project-job-scheduling-compare --seconds 10 --workers 8` in
`benchmarks/project-job-scheduling/cpsat`
([README](../../../benchmarks/project-job-scheduling/README.md)); the same project has
`project-job-scheduling-public-bench` for the public RG30 instances. All three run the
default path, so they show a constraint after step 5.

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/pjs/constraints/<subject>_cost.hpp` | new | | member-change total | weight |
| `cpp/include/pjs/constraints.hpp` | include, `emplace` | | | |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | counts in the text |
| `tests/project_job_scheduling/cpp/pjs_plugin_test.cpp` | | cases, counts | | the row, expected scores |
| `tests/project_job_scheduling/test_constraint_specs.py`, `test_constraints.py` | | lists | | the row, expected scores |
| `docs/use-cases/project-job-scheduling/README.md`, `python.md` | | table row | | defaults |
| `cpp/src/score_calculator.cpp` | | | | the term |
| `cpp/src/incremental_calculator.cpp`, `cpp/include/pjs/incremental_calculator.hpp` | | | | the total |
| `tests/project_job_scheduling/cpp/pjs_test.cpp` | | | | expected scores |
| external-solver, `benchmarks/project-job-scheduling` | | | | the same constraint, results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

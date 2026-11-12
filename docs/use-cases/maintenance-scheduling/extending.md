# Maintenance scheduling: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its six constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | two C++ test cases, five checks that count or list the catalog |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | both monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `weekendSpanCost`, the Saturdays and Sundays a job stays open across,
between its first and its last working day. A job left open over a weekend keeps its site out of
service on days nobody works on it. Every snippet below was compiled and run, and every figure
measured, on optimaru 0.4.3; a later change to the solver moves the figures, not the steps.

## How a schedule is scored

A job is assigned a crew and a start date, packed into one value, `crewDate` (see
[Domain encoding](README.md#domain-encoding)). The score is kept per **unit**: one
`crewDate`, a (start date, crew) cell of the grid, with the jobs placed there as its
**members**. An unassigned job belongs to no unit and scores nothing. A move changes
one or two jobs, so only the cells they leave and join are scored again.

There are two scoring paths, and they give the same score for the same schedule
(`mnt_plugin_test` checks it on 400 random assignments):

| | `mnt.solve(schedule)` | `mnt.solve(schedule, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/mnt/constraints/` |
| Constraints | the six, at their defaults | the ones you list, with your knobs |
| Steps per second | 5.8 million | 3.8 million |
| Parallel starts | `parallel_solver_count` | one start |
| Guided moves | `window_value_probability`, `hot_job_probability` | none |

The speeds are the benchmark's `large` instance (48 jobs, 5 crews, 80 start dates),
2 million steps, one thread of an Apple M4 Max; [step 6](#6-measure) has the script.
The guided moves (a start date from the job's own window, a job from a tag conflict)
read live state of the monolithic calculator, so the assembly searches without them
([`solver.cpp`](../../../optimaru/maintenance_scheduling/cpp/src/solver.cpp)). Steps 1
to 4 work on the assembly. Step 5 moves a constraint to the monolithic calculators.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import optimaru.maintenance_scheduling as mnt
from optimaru.maintenance_scheduling.constraints import AfterIdealEndDateCost, TagConflictCost, get_constraints

schedule = mnt.generate_demo_data("LARGE", 20_454)  # a fixed Monday; the default is next Monday
changed = {"after_ideal_end_date_cost", "tag_conflict_cost", "before_ideal_end_date_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    AfterIdealEndDateCost(weight=10_000),                               # lighter (default 1,000,000)
    TagConflictCost(level="hard"),                                      # soft by default
]                                                                       # before_ideal_end_date_cost: dropped
result = mnt.solve(schedule, time_limit_seconds=10, constraints=picks)
```

To try weights without solving again, score a finished schedule with other picks:

```python
from optimaru.maintenance_scheduling.constraints import score

raw = mnt.constraint_costs(result, picks)  # raw cost of every constraint, per cell or one total
print(score(raw, picks))                   # the score those picks give this schedule
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/maintenance_scheduling/constraints.py).
`level` is `"hard"` or `"soft"`: this use case has no medium level, and
`level="medium"` raises `ValueError`. Stop here if the constraint you need is one of the six.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

[`constraints/common.hpp`](../../../optimaru/maintenance_scheduling/cpp/include/mnt/constraints/common.hpp)
has two bases, one per kind of constraint here. Pick by what the constraint compares:

| The constraint | Base | You write | Example to copy |
| --- | --- | --- | --- |
| a sum of one term per job, from the job and its cell (crew, start date, end date) | `PerJobConstraint` | `jobCost(schedule, unit, jobIndex)` | [`before_ideal_end_date_cost.hpp`](../../../optimaru/maintenance_scheduling/cpp/include/mnt/constraints/before_ideal_end_date_cost.hpp) |
| jobs that share an owner (a crew, a tag) on the same day, which spans cells | `DayOverlapGlobalConstraint` | `ownerCount()`, `ownersOf()` | [`tag_conflict_cost.hpp`](../../../optimaru/maintenance_scheduling/cpp/include/mnt/constraints/tag_conflict_cost.hpp) |

The four date constraints use the first, the two conflict constraints the second. A constraint that
fits neither is the engine's `UnitConstraint` or `GlobalConstraint` directly ([Extending a use
case](../../extending.md)). `detail::occupancyOf()` in `common.hpp` gives the `[start, end)` days a
job covers in a cell; the end date is exclusive and skips weekends, so a five-day job from a Monday
ends on the Saturday.

The example is a term per job. Four edits:

**1. The component**, `optimaru/maintenance_scheduling/cpp/include/mnt/constraints/weekend_span_cost.hpp`:

```cpp
#ifndef MNT_CONSTRAINTS_WEEKEND_SPAN_COST_HPP
#define MNT_CONSTRAINTS_WEEKEND_SPAN_COST_HPP

#include "mnt/constraints/common.hpp"

namespace mnt {

// Soft, off by default (weight 0): weekendSpanCost — the weekend days a job
// stays open across. Raw cost per (start date, crew) cell = the summed
// calendar days its jobs span beyond their working days, i.e. the Saturdays
// and Sundays between a job's first and last working day; 0 = every job
// there starts and ends within one working week. A job left open over a
// weekend keeps its site out of service on days nobody works on it.
struct WeekendSpanCost final : PerJobConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit WeekendSpanCost(double weight = kDefaultWeight)
        : PerJobConstraint("weekendSpanCost", weight, kDefaultLevel) {}
    double jobCost(const MaintenanceSchedule& schedule, int unit, int jobIndex) const override {
        std::int64_t startDate = 0;
        std::int64_t endDate = 0;
        detail::occupancyOf(schedule, unit, jobIndex, startDate, endDate);
        return static_cast<double>(endDate - startDate - schedule.jobs[jobIndex].durationInDays);
    }
};

}  // namespace mnt

#endif  // MNT_CONSTRAINTS_WEEKEND_SPAN_COST_HPP
```

The name passed to the base, `"weekendSpanCost"`, is the struct name with a lower-case
first letter. Python derives `weekend_span_cost` from it.

The defaults live in these two constants and nowhere else; Python reads them through
`mnt.constraint_info()`, and so does the CP-SAT model of the benchmark. `kDefaultLevel`
is `Hard` or `Soft`; the domain maps 1:1 onto the engine's score (`int64` hard, `double`
soft) and has no medium level to fold. The weight is in units of that level, and the
soft weights of the six constraints make a ladder: a day early costs 1, a tag-conflict day
1,000, a day late 1,000,000. A new soft constraint takes its place on that ladder by its
weight. A hard constraint returns whole numbers, because a hard contribution is rounded per
cell, and every constraint here counts days, so the soft total stays exact as well.

**2. The assembly**, `cpp/include/mnt/constraints.hpp`: include the header, and add one
line at the end of `defaultConstraints()`:

```cpp
    assembly.emplace<WeekendSpanCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no default solve changes and the monolithic calculators stay as they are.
`list_constraints()` lists the per-job constraints first and the two global ones after them,
wherever the `emplace` line sits.

**3. The Python spec**, `optimaru/maintenance_scheduling/constraints.py`:

```python
class WeekendSpanCost(Constraint):
    """Weekend days a job stays open across. Opt-in: soft, weight 0."""

    name = "weekend_span_cost"
    label_en = "Jobs open over a weekend"
    label_ko = "주말에 걸친 작업"
    description_en = (
        "Weekend days between the first and the last working day of an assigned job, summed over the "
        "jobs. One unit of cost is one Saturday or Sunday a job stays open across; 0 means every job "
        "starts and ends within one working week."
    )
    description_ko = (
        "배정된 작업마다 첫 작업일과 마지막 작업일 사이에 낀 주말 날수를 모두 더합니다. 작업이 걸쳐 있는 "
        "토요일이나 일요일 하루가 비용 1이며, 0이면 모든 작업이 한 주의 평일 안에서 시작하고 끝납니다."
    )
```

**4. Its default instance**, in the same file: `weekend_span_cost = WeekendSpanCost()`
with the other instances, that instance in `_DEFAULTS` before `crew_conflict_cost` (the
per-job constraints come first, the two globals last, as `list_constraints()` reports them),
and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=maintenance_scheduling
uv sync                                    # rebuilds optimaru.maintenance_scheduling._core
```

```python
from optimaru.maintenance_scheduling.constraints import WeekendSpanCost

others = [spec for spec in get_constraints() if spec.name != "weekend_span_cost"]
picks = [*others, WeekendSpanCost(weight=10)]
config = mnt.SolverConfig()
config.step_count_limit = 2_000_000
config.random_seed = 1
result = mnt.solve(mnt.generate_demo_data("LARGE", 20_454), config=config, constraints=picks)
print(sum(mnt.constraint_costs(result, picks)["weekend_span_cost"]["costs"]))
```

On the `large` instance that sum is 56. A default solve of the same length
(`constraints=None`) leaves it at 88, 44 weekends: at weight 10 a weekend day costs
as much as ten days of finishing early.

What the base gave you: `cost()` of a cell is the sum of `jobCost` over its jobs, and
`delta_cost()` is `jobCost` of the job that joins or leaves. When a job moves, the
engine calls `jobCost` once for the cell it leaves and once for the cell it joins. The
constraint is already incremental.

## 3. Check it

**Cases worked out by hand**, in `tests/maintenance_scheduling/cpp/mnt_plugin_test.cpp`.
One next to the other single-component cases in `verifyPartialAssemblies()`:

```cpp
    {  // weekendSpanCost alone: 3 working days from a Thursday are Thu, Fri
        // and Mon, so the job stays open over one weekend: 2 days, at
        // weight 3.
        MaintenanceSchedule s = makeBase();
        addJob(s, 3);
        ScoreCalculator::prepare(s);
        s.jobs[0].crewDate = s.crewDateOf(3, 0);  // DAY_1 + 3, a Thursday
        DecomposedCalculator calculator;
        calculator.emplace<WeekendSpanCost>(3.0);
        checkScore("weekendSpanCost alone (3 * 2)", scoreWith(calculator, s), 0, -6.0);
    }
```

And, in `verifyRawCostsByHand()`, a violating and a clean case of the raw cost per cell,
as every constraint has there: six working days from a Wednesday cost 2 in their cell,
five from a Monday cost 0.

```sh
cmake -B build -DOPTIMARU_USE_CASES=maintenance_scheduling && cmake --build build -j
ctest --test-dir build --output-on-failure
```

**The counts.** A seventh constraint fails five checks that count or list the catalog.
Update them:

- `mnt_plugin_test.cpp`: `constraint_totals().size() == 6` in
  `verifyConstraintTotals()`, and `rows.size() == 6` in `verifyCatalog()`. Add the new
  row there, before `crewConflictCost`. `rowIs()` expects a constraint that is on by default,
  so check the new row's fields directly (`!rows[4].default_enabled`);
- `tests/maintenance_scheduling/test_constraint_specs.py`:
  `test_list_constraints_matches_the_spec_names` (the list),
  `test_module_level_instances_keep_every_default` (the instances) and
  `test_constraint_info_reports_the_cpp_defaults` (the rows).

`test_offset_zero_reproduces_the_plain_scores` picks every default, so it covers the new
constraint without a change.

**Incremental against full.** Solve with every score checked against a full recompute. It raises
`RuntimeError` ("Score corruption: ...", the engine's `ScoreMismatchError`) if a move leaves the
score stale:

```python
mnt.solve(schedule, time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

Run it once more with the constraint picked twice (`WeekendSpanCost(weight=10),
WeekendSpanCost(weight=1, exponent=2)`). `mnt_plugin_test` makes the same comparison
on the default assembly (3,000 random moves, and a 2-second solve under
`ScoreChecks::Full`), where the constraint is at weight 0, so its value is not part of it.

**The tables.** The constraint table in [README.md](README.md) must equal
`mnt.constraint_info()`; add the row, and the file to the component list under
[C++](README.md#c). The notebook `examples/maintenance-scheduling.ipynb` prints the
constraint list.

## 4. Make it cheaper per move

On the assembly a move costs, for every constraint, one call for each cell a job leaves
and each it joins, and as many again when a rejected move is undone. In 2 million
steps on the `large` instance the example's `jobCost` ran 10.4 million times, 5.2
times a step, picked at weight 0 or at 10 alike: a constraint at weight 0 is still called,
it only adds nothing. So leave a constraint out of the picks rather than give it weight 0;
`get_constraints()` includes it.

What one call costs depends on the constraint:

| The constraint is | Use | Per move | Example to copy |
| --- | --- | --- | --- |
| a sum of per-job terms | `PerJobConstraint` | one `jobCost` call | step 2 |
| a pair count over jobs that share an owner on a day | `DayOverlapGlobalConstraint`: the base keeps a count per (owner, day) | the days of the job, times its owners | [`crew_conflict_cost.hpp`](../../../optimaru/maintenance_scheduling/cpp/include/mnt/constraints/crew_conflict_cost.hpp) |
| any other statistic across cells | `MemberChangeGlobalConstraint`: keep your own totals in `on_member_changed()` | what your update costs | `DayOverlapGlobalConstraint` in [`common.hpp`](../../../optimaru/maintenance_scheduling/cpp/include/mnt/constraints/common.hpp) |
| several per-job constraints that read the same facts | one compiled evaluator for the group (`delta_group_key()`, `create_delta_group()`); not used here | one pass for the group | employee scheduling's [`assignment_delta_group.hpp`](../../../optimaru/employee_scheduling/cpp/include/es/constraints/assignment_delta_group.hpp) |

A pair constraint written as a unit constraint with `cost()` alone would be wrong here, not
just slow: two jobs of one crew that start on different dates sit in different cells,
so the overlap is not inside one unit. That is why both conflict constraints are global.

**Compute once per solve.** The facts never change during a solve, so a term that
reads only facts can be a table. `on_reset()` runs when the objective is rebuilt:
twice per solve here, at the start and for the final score. The example's term reads
the job's duration and its start date, so the table is jobs times start dates:

```cpp
    // One table per rebuild: jobCost() below is then an array read.
    void on_reset(const MaintenanceSchedule& schedule) override {
        PerJobConstraint::on_reset(schedule);
        dateCount_ = static_cast<std::size_t>(schedule.dateCount());
        table_.assign(schedule.jobs.size() * dateCount_, 0.0);
        for (std::size_t job = 0; job < schedule.jobs.size(); ++job) {
            const int duration = schedule.jobs[job].durationInDays;
            for (std::size_t date = 0; date < dateCount_; ++date) {
                const std::int64_t span = schedule.endDateFor(duration, static_cast<int>(date)) -
                                          schedule.startDates[date];
                table_[job * dateCount_ + date] = static_cast<double>(span - duration);
            }
        }
    }
    double jobCost(const MaintenanceSchedule& schedule, int unit, int jobIndex) const override {
        const std::size_t date = static_cast<std::size_t>(schedule.dateIndexOf(unit));
        return table_[static_cast<std::size_t>(jobIndex) * dateCount_ + date];
    }

private:
    std::size_t dateCount_ = 0;
    std::vector<double> table_;  // jobs x start dates
```

With the constraint at weight 10 the assembly went from 3.46 to 3.50 million steps per
second, and the solve ended on the same score, as it must. Picked at weight 0 it went
from 3.35 to 3.46, against 3.75 with the constraint left out. The gain is small: the term was
already a few array reads (`occupancyOf()` reads `startDates` and `endDateByDur`); a
term that walks the job's days or its tags gains more.

**Say which cells the constraint can charge.** `scoped_units()` returns the units a constraint is
ever non-zero for, and the engine skips it on the others. `PerJobConstraint` keeps
the default, `nullptr`: every cell. A constraint about one crew could return its cells.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`mnt.solve(schedule)` must score it, and `mnt.solve(schedule)` scores with the
monolithic calculators: 1.5 times the steps per second of the assembly, the only
path that runs parallel starts, and the one with guided moves.

`mnt_plugin_test` compares the default assembly with the monolithic calculators on
random assignments, so it fails from the moment the weight is not 0 until both sides
agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 10.0;`.
It must be a whole number, because the monolithic calculators add in `int64`.

**2. The term, twice.** Both calculators read the weight off the component, as they do
for the six: include the header, add `constexpr auto kWeekendWeight =
static_cast<std::int64_t>(WeekendSpanCost::kDefaultWeight);`, and add the weight and the
level to the two `static_assert`s. `ScoreCalculator::score()` in
`cpp/src/score_calculator.cpp` is the full recompute. In its loop over the jobs, after
the ideal-date terms, with `std::int64_t weekendSpan = 0;` declared before the loop:

```cpp
        // soft 7: the weekend days between the job's first and last working day.
        weekendSpan += endDate - startDate - job.durationInDays;
```

and `+ kWeekendWeight * weekendSpan` in `score.soft`.

`cpp/src/incremental_calculator.cpp` is what a move pays: `insert()` adds a job's terms
when it joins a cell and `retract()` takes them out when it leaves. Add a running total
`std::int64_t weekendSpan_ = 0;` to `incremental_calculator.hpp` next to `afterIdeal_`,
zero it in `rebuild()`, and:

```cpp
    // soft 7: the weekend days between the job's first and last working day.
    weekendSpan_ += endDate - startDate - job.durationInDays;
```

in `insert()`, the same line with `-=` in `retract()`, and `+ kWeekendWeight *
weekendSpan_` in `score()`. A constraint over pairs goes into the per-day counters of the same
two functions instead, as `crewOverlap_` and `tagOverlap_` do.

The default path ran at 6.05 million steps per second with the seventh constraint in it
(5.78 with six). The constraint changes where the search goes, and that moves the speed more
than the term costs. With the guidance off it ended on the score the assembly gave at
weight 10.

**3. What else moves.** A constraint that is on by default changes every default score:

- six checks failed here. `mnt_plugin_test.cpp`: the catalog row of step 3, and
  `verifyAssemblyBuilder()`, whose assembly "listed by hand" has six constraints.
  `mnt_test.cpp`, the hand-computed scores of the monolithic calculators: "crewConflictCost
  overlap counts weekend days" (two six-day jobs from a Friday, soft 0 becomes -40) and
  "endDate skips weekend (after)" (-1,000,000 becomes -1,000,020).
  `test_constraint_specs.py`: `test_constraint_info_reports_the_cpp_defaults`, and
  `test_weight_none_means_the_component_default` with its six explicit weights;
- the tables, the score formula and the "six" in [README.md](README.md),
  [python.md](python.md), `constraints.py`, `__init__.py`, `common.hpp`,
  `constraints.hpp`, `score_calculator.hpp` and the soft-weight note in `domain.hpp`;
- the CP-SAT model the benchmark compares against,
  [`external_solver.maintenance_scheduling.constraints`](https://github.com/optimaru/external-solver/blob/v0.3.0/src/external_solver/maintenance_scheduling/constraints.py)
  in external-solver: plugins that subclass the Python specs and take their defaults from
  `constraint_info()`. The four date constraints share `_DateBoundaryPlugin`, one term per (crew,
  start date) cell from `raw(job, start, end)`; the example is one more boundary there. Without the
  same constraint there,
  [`benchmarks/maintenance-scheduling`](../../../benchmarks/maintenance-scheduling) no longer
  compares one objective;
- the published results in `benchmarks/maintenance-scheduling/README.md`, and
  `CHANGELOG.md`.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed:

```python
import time

import optimaru.maintenance_scheduling as mnt
from optimaru.maintenance_scheduling.constraints import get_constraints

steps = 2_000_000
for label, picked, guided in (
    ("default path", None, True),
    ("default, unguided", None, False),
    ("assembly", get_constraints(), True),
):
    config = mnt.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    if not guided:  # the assembly has no guided moves
        config.window_value_probability = 0.0
        config.hot_job_probability = 0.0
    schedule = mnt.generate_demo_data("LARGE", 20_454)
    start = time.perf_counter()
    result = mnt.solve(schedule, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:18s} {steps / seconds / 1e6:5.2f} M steps/s  {result.score}")
```

The last two lines end on the same score: with the guidance off, the default path
searches like the assembly, so same seed, same steps, same scoring. If they differ
after a change, the two paths no longer score alike. The first line is the default
path as it ships; it ends elsewhere because it searches differently.

| Measured on `large` | Million steps per second |
| --- | --- |
| default path, six constraints | 5.78 |
| default path, six constraints, guidance off | 6.32 |
| assembly, six constraints | 3.75 |
| assembly, six + the constraint of step 2 at weight 0 | 3.35 |
| assembly, six + the constraint at weight 10 | 3.46 |
| assembly, six + the constraint at weight 0, with the table of step 4 | 3.46 |
| assembly, six + the constraint at weight 10, with the table of step 4 | 3.50 |
| default path, seven constraints (step 5) | 6.05 |
| default path, seven constraints, guidance off | 6.58 |

Each is the median of three runs of 2 million steps on one thread of an Apple M4 Max.
`large` is `mnt.generate_demo_data("LARGE", 20_454)`, the `large` instance of
[`benchmarks/maintenance-scheduling`](../../../benchmarks/maintenance-scheduling).

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
scores at the same budget over several seeds, before and after:

```python
for seed in range(1, 6):
    result = mnt.solve(mnt.generate_demo_data("LARGE", 20_454), time_limit_seconds=5, random_seed=seed)
    print(seed, result.score)
```

This use case has no C++ quality bench (`mnt_demo` solves the demo for a number of
seconds and checks hard 0). Run the loop from two checkouts, each with its own build,
with `constraints=None` for the default path after step 5 or your picks before it.
Against CP-SAT: `uv run maintenance-scheduling-compare --seconds 10 --workers 8` in
`benchmarks/maintenance-scheduling/cpsat`
([README](../../../benchmarks/maintenance-scheduling/README.md)).

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/mnt/constraints/<subject>_cost.hpp` | new | | table | weight |
| `cpp/include/mnt/constraints.hpp` | include, `emplace` | | | the list in the comment |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | counts in the text |
| `tests/maintenance_scheduling/cpp/mnt_plugin_test.cpp` | | cases, counts | | catalog row, builder |
| `tests/maintenance_scheduling/test_constraint_specs.py` | | lists | | defaults |
| `docs/use-cases/maintenance-scheduling/README.md`, `python.md` | | table row | | defaults, score |
| `cpp/src/score_calculator.cpp`, `cpp/src/incremental_calculator.cpp`, `incremental_calculator.hpp` | | | | the term |
| `tests/maintenance_scheduling/cpp/mnt_test.cpp` | | | | expected scores |
| external-solver, `benchmarks/maintenance-scheduling` | | | | the same constraint, results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

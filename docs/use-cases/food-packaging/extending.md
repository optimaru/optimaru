# Food packaging: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its five constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | two C++ test cases, five checks that count or list the catalog |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the picks; nothing in this constraint |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | both monolithic calculators and their headers |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `beforeIdealEndTimeCost`, the minutes a job is done before its ideal
end time, the mirror of `afterIdealEndTimeCost`. What is packaged early has to be stored until it is
due. Every snippet below was compiled and run, and every figure measured, on optimaru 0.5.1; a later
change to the solver moves the figures, not the steps.

## How a schedule is scored

Every line has an ordered list of jobs. Jobs on no line are unassigned, and the engine
sees them as one more list, the unassigned pool. The score is kept per **unit**: one
list, a line or the pool, with its jobs in production order as its **members**. A move
changes one or two lists, and only those are scored again, each as a whole.

There are two scoring paths, and they give the same score for the same schedule
(`fp_plugin_test` checks it on 200 random list layouts):

| | `fp.solve(schedule)` | `fp.solve(schedule, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/fp/constraints/` |
| Constraints | the five, at their defaults | the ones you list, with your knobs |
| Steps per second | 2.7 million | 0.26 million |
| Parallel starts | `parallel_solver_count` | one start |
| Operators of the lines | reassigned between search legs when lines share one (`operator_rounds`) | assigned once, one leg |

The speeds are the demo plant (100 jobs, 5 lines), 500,000 steps, one thread of an
Apple M4 Max; [step 6](#6-measure) has the script and a larger plant. Steps 1 to 4 work
on the assembly. Step 5 moves a constraint to the monolithic calculators.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import optimaru.food_packaging as fp
from optimaru.food_packaging.constraints import AfterIdealEndTimeCost, UnassignedJobsCost, get_constraints

schedule = fp.generate_demo_data()
changed = {"after_ideal_end_time_cost", "unassigned_jobs_cost", "operator_cleaning_conflict_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    AfterIdealEndTimeCost(weight=3),                                    # heavier (default 1)
    UnassignedJobsCost(level="hard"),                                   # medium by default
]                                                                       # operator_cleaning_conflict_cost: dropped
fp.solve(schedule, time_limit_seconds=10, constraints=picks)            # solves the schedule in place
```

To try weights without solving again, score a finished schedule with other picks:

```python
from optimaru.food_packaging.constraints import score

raw = fp.constraint_costs(schedule, picks)  # raw cost of every constraint, per line
print(score(raw, picks))                    # the score those picks give this schedule
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/food_packaging/constraints.py).
Stop here if the constraint you need is one of the five.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

[`constraints/common.hpp`](../../../optimaru/food_packaging/cpp/include/fp/constraints/common.hpp)
has a base for the constraints of one line and a walk that derives the job times. Pick by
what the constraint reads:

| The constraint | Build on | You write | Example to copy |
| --- | --- | --- | --- |
| reads the jobs of one line and their times | `detail::PerLineConstraint` (a `UnitConstraint` of the engine) and `detail::walkChain()` | `cost()` with a callback per job | [`after_ideal_end_time_cost.hpp`](../../../optimaru/food_packaging/cpp/include/fp/constraints/after_ideal_end_time_cost.hpp) |
| spans lines, or counts the jobs on no line | `FpGlobalConstraint` (the engine's `GlobalConstraint`) | `reset()`, `on_unit_changed()`, `cost()` | [`unassigned_jobs_cost.hpp`](../../../optimaru/food_packaging/cpp/include/fp/constraints/unassigned_jobs_cost.hpp) |

The example reads one line, so it is one function. Four edits:

**1. The component**, `optimaru/food_packaging/cpp/include/fp/constraints/before_ideal_end_time_cost.hpp`:

```cpp
#ifndef FP_CONSTRAINTS_BEFORE_IDEAL_END_TIME_COST_HPP
#define FP_CONSTRAINTS_BEFORE_IDEAL_END_TIME_COST_HPP

#include "fp/constraints/common.hpp"

namespace fp {

// Soft, off by default (weight 0): beforeIdealEndTimeCost — the minutes a job
// is done before the time it should ideally be done by (Job::idealEndTime),
// the mirror of afterIdealEndTimeCost. Raw cost per line = the sum over its
// jobs of max(0, idealEndTime - end), so 0 = no job on the line ends early.
// What is packaged early has to be stored until it is due.
struct BeforeIdealEndTimeCost final : detail::PerLineConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit BeforeIdealEndTimeCost(double weight = kDefaultWeight)
        : detail::PerLineConstraint("beforeIdealEndTimeCost", weight, kDefaultLevel) {}
    double cost(const PackagingSchedule& schedule, int unit,
                const std::vector<int>& /*members*/) const override {
        if (detail::isPool(schedule, unit)) return 0.0;
        std::int64_t early = 0;
        detail::walkChain(schedule, unit, detail::jobsOfUnit(schedule, unit),
                          [&](int jobIndex, Minutes /*startCleaning*/, Minutes /*startProduction*/,
                              Minutes end) {
                              const Job& job = schedule.jobs[jobIndex];
                              if (end < job.idealEndTime) early += job.idealEndTime - end;
                          });
        return static_cast<double>(early);
    }
};

}  // namespace fp

#endif  // FP_CONSTRAINTS_BEFORE_IDEAL_END_TIME_COST_HPP
```

The name passed to the base, `"beforeIdealEndTimeCost"`, is the struct name with a
lower-case first letter. Python derives `before_ideal_end_time_cost` from it.

Three things here are this use case's own:

- `cost()` does not read `members`. The engine hides the pinned jobs at the head of a
  line and numbers the rest its own way, so `members` is neither the whole line nor a
  list of job indices. `detail::jobsOfUnit(schedule, unit)` is the line as the schedule
  holds it, in production order.
- `detail::walkChain()` gives the cleaning start, the production start and the end of
  every job. A constraint must not read `Job::endDateTime` and the other derived fields: the
  search does not keep them up to date.
- `PerLineConstraint` scopes the constraint to the lines, so the pool is never asked.

The defaults live in the two constants and nowhere else; Python reads them through
`fp.constraint_info()`. `kDefaultLevel` is `Hard`, `Medium` or `Soft`, and the weight is
in units of that level. A medium constraint states its weight in medium units
(`afterIdealEndTimeCost`, `unassignedJobsCost`: 1): the domain folds the medium level
into the engine's score itself (`kMediumFold` in `domain.hpp`:
`engine.soft = medium * 10^10 + soft`), so no weight carries the 10^10. A hard constraint
returns whole numbers, because a hard contribution is rounded per line.

**2. The assembly**, `cpp/include/fp/constraints.hpp`: include the header, and add
one line at the end of `defaultConstraints()`:

```cpp
    assembly.emplace<BeforeIdealEndTimeCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no default solve changes and the monolithic calculators stay as they are.

**3. The Python spec**, `optimaru/food_packaging/constraints.py`:

```python
class BeforeIdealEndTimeCost(Constraint):
    """Minutes by which a job is done before its ideal end time. Opt-in: soft, weight 0.

    The mirror of `after_ideal_end_time_cost`: what is packaged early has to be stored."""

    name = "before_ideal_end_time_cost"
    label_en = "Job done before its ideal end time"
    label_ko = "이상적 종료 시각보다 이른 종료"
    description_en = (
        "Minutes by which an assigned job ends before its ideal end time (ideal_end_time), summed "
        "over the jobs of every line. One unit of cost is one minute early; 0 means no assigned job "
        "ends before its ideal time."
    )
    description_ko = (
        "라인에 배정된 작업이 이상적인 종료 시각(ideal_end_time)보다 일찍 끝난 시간(분)을 모든 라인에 걸쳐 "
        "더합니다. 1분 일찍 끝날 때마다 비용 1이며, 0이면 이상적인 종료 시각보다 일찍 끝나는 작업이 없습니다."
    )
```

**4. Its default instance**, in the same file: `before_ideal_end_time_cost =
BeforeIdealEndTimeCost()` with the other instances, that instance in `_DEFAULTS`
after `squared_makespan_cost` (the per-line constraints come first, the two globals last,
as `list_constraints()` reports them), and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=food_packaging
uv sync                                    # rebuilds optimaru.food_packaging._core
```

```python
from optimaru.food_packaging.constraints import BeforeIdealEndTimeCost

others = [spec for spec in get_constraints() if spec.name != "before_ideal_end_time_cost"]
picks = [*others, BeforeIdealEndTimeCost(weight=1000)]
config = fp.SolverConfig()
config.step_count_limit = 500_000
config.random_seed = 1
schedule = fp.generate_demo_data()
fp.solve(schedule, config=config, constraints=picks)
print(sum(fp.constraint_costs(schedule, picks)["before_ideal_end_time_cost"]["costs"]))
```

On the demo plant that sum is 75,210 minutes. A default solve of the same length
(`constraints=None`) leaves it at 186,251. The price is on the same soft level: the
squared makespans grow from 97 to 148 million, because a job can only end later by
queueing behind more jobs.

What the engine does with the one function: when a move changes a line's list, it
calls `cost()` for that line and replaces what the line contributed. Nothing else is
asked of the constraint.

## 3. Check it

**Cases worked out by hand**, in `tests/food_packaging/cpp/fp_plugin_test.cpp`, next
to the other raw costs in `verifyRawCostsByHand()`: one violating and one clean, as
every constraint has there. `rawCosts()` returns one cost per list, the lines and then
the pool:

```cpp
    {
        // Line 0: J0 ends 200 (ideal 500) -> 300 early, J1 ends 350 (ideal
        // 400) -> 50 early. Line 1's job ends at 100, exactly when it is due.
        PackagingSchedule s = handSchedule(2);
        addLine(s, 0, {{PA_S, 200, 500, BIG}, {PA_S, 150, 400, BIG}});
        addLine(s, 1, {{PB, 100, 100, BIG}});
        check(rawCosts(s, "beforeIdealEndTimeCost") == Costs{350.0, 0.0, 0.0},
              "beforeIdealEndTimeCost: [300 + 50, 0, pool 0]");
    }
    {
        // The same chain with each job due no later than it ends.
        PackagingSchedule s = handSchedule(2);
        addLine(s, 0, {{PA_S, 200, 200, BIG}, {PA_S, 150, 250, BIG}});
        addLine(s, 1, {{PB, 100, 100, BIG}});
        check(rawCosts(s, "beforeIdealEndTimeCost") == Costs{0.0, 0.0, 0.0},
              "beforeIdealEndTimeCost: [0, 0, 0] no job ends before its ideal time");
    }
```

```sh
cmake -B build -DOPTIMARU_USE_CASES=food_packaging && cmake --build build -j
ctest --test-dir build --output-on-failure
```

**The counts.** A sixth constraint fails five checks that count or list the catalog.
Update them:

- `fp_plugin_test.cpp`: `constraint_totals().size() == 5` in `verifyConstraintTotals()`,
  and `rows.size() == 5` in `verifyCatalog()`. Add the new row there, before
  `operatorCleaningConflictCost`. `rowIs()` expects a constraint that is on by default, so
  check the new row's fields directly (`!rows[3].default_enabled`);
- `tests/food_packaging/test_constraint_specs.py`:
  `test_list_constraints_matches_the_spec_names` (the list), and `EXPECTED_INFO`, which
  `test_constraint_info_reports_the_cpp_defaults` and
  `test_weight_none_means_the_component_default` read. The first of the two also
  asserts that every row is on by default.

**Incremental against full.** Solve with every score checked against a full recompute. It raises
`RuntimeError` ("Score corruption: ...", the engine's `ScoreMismatchError`) if a move leaves the
score stale:

```python
schedule = fp.generate_demo_data()
fp.solve(schedule, time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

Run it once more with the constraint picked twice (`BeforeIdealEndTimeCost(weight=1000),
BeforeIdealEndTimeCost(weight=1, exponent=2)`): the engine computes the raw cost of a
repeated pick once, and this checks that too. The 1,500 random moves of
`fp_plugin_test` run on the default assembly, where the constraint is at weight 0 and asked
on no move, so they say nothing about it.

**The tables.** The constraint tables in [README.md](README.md) and
[python.md](python.md) must equal `fp.constraint_info()`; add the row, and the file to
the list in [cpp.md](cpp.md). The notebook `examples/food-packaging.ipynb` prints the
constraint list, and the docstrings of `_core.pyi` name the constraints.

## 4. Make it cheaper per move

On the assembly a constraint with a weight pays one `cost()` call for a line each time the
engine edits that line's list. A move is several edits, and a rejected move is undone with
as many again: in 500,000 steps on the demo plant the example's `cost()` ran 2.05 million
times at weight 1000, 4.1 times a step. Each call is one walk of the line. That is as
cheap as a constraint of one line gets here, and it is not where the time goes (the pair
scan below is): with the constraint at weight 1000 the assembly ran at 0.24 million steps
per second, against 0.26 without it, and part of that is the search, which now has early
ends to trade. A constraint at weight 0 is asked on no edit: only a rebuild of the
objective costs it, and `constraint_costs()` when it reports, so the assembly with it runs
as the assembly without it.

What each kind of constraint costs, and the one thing that changes the speed of the assembly:

| The constraint is | Use | Per edit of a list | Example to copy |
| --- | --- | --- | --- |
| a sum over one line's jobs | `PerLineConstraint` and `walkChain()` | one walk of that line | step 2 |
| a total over the lines | `FpGlobalConstraint` that keeps one number per line in `on_unit_changed()` | one pass over that line | [`unassigned_jobs_cost.hpp`](../../../optimaru/food_packaging/cpp/include/fp/constraints/unassigned_jobs_cost.hpp) |
| about pairs of jobs on different lines | `FpGlobalConstraint` that marks itself stale and recomputes in `cost()` | every job, and every pair of jobs, at every score | [`operator_cleaning_conflict_cost.hpp`](../../../optimaru/food_packaging/cpp/include/fp/constraints/operator_cleaning_conflict_cost.hpp) |

**Leave out a constraint that cannot fire.** `operatorCleaningConflictCost` is the third
kind, and it sets the pace of the whole assembly. It is 0 by construction when every
line has an operator of its own, which is the demo plant: 5 lines, 5 operators. Picks
without it ran at 2.2 million steps per second instead of 0.26, and ended on the same
score. Measure a new constraint that way, or its cost is lost in the noise of the pair scan.
A new constraint over pairs should keep per-line state and compare only the lines a move
changed, as `incremental_calculator.cpp` does for this one (`pairOverlap()`).

**Compute once per solve.** A term that reads only facts can be a table built in
`on_reset()`. Call `PerLineConstraint::on_reset()` first: it builds the scope.
`on_reset()` ran 3 times in the 500,000 steps (at the start, when the best schedule is
put back at the end, and for the final score). The engine also rebuilds after every
rejected compound move, but this use case switches none of them on. Under
`full_assert` it rebuilds on every step.

**No move previews here.** The engine can ask an assembly what a move would score
without applying it, by calling `cost()` on the candidate members. This assembly
cannot answer: its constraints read the line from the schedule, which a preview has not
changed, and neither global implements `supports_preview()`. So every move is applied,
scored and, when rejected, undone, which is the 4.1 calls a step above.

**Keep raw costs whole numbers.** Every constraint here counts minutes, so sums stay exact
and a hard total is never rounded. Return `static_cast<double>` of an integer count.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`fp.solve(schedule)` must score it, and `fp.solve(schedule)` scores with the monolithic
calculators: 10 times the steps per second of the assembly on the demo plant, and the
only path that runs parallel starts and reassigns operators between legs.

`fp_plugin_test` compares the default assembly with the monolithic calculators on
random layouts and random moves, so it fails from the moment the weight is not 0 until
both sides agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 1000.0;`.
It must be a whole number, because the monolithic calculators add in `int64`, and it
must keep the soft total below 10^10, where the medium level starts (`domain.hpp`).

**2. The full recompute**, `cpp/src/score_calculator.cpp` and its header. Include the
component, read its weight next to the others (`kBeforeIdealEndWeight`), add it and
its level to the two `static_assert`s, give `ConstraintBreakdown` a member named after
the constraint, add `kBeforeIdealEndWeight * beforeIdealEndTimeCost` to the soft level in
`toScore()`, and add the term to the loop over the jobs in `ScoreCalculator::analyze()`:

```cpp
        // beforeIdealEndTimeCost (soft): the mirror image.
        if (job.endDateTime < job.idealEndTime)
            breakdown.beforeIdealEndTimeCost += job.idealEndTime - job.endDateTime;
```

**3. The incremental calculator**, `cpp/src/incremental_calculator.cpp` and its
header. It keeps one `LineState` per line and re-derives it with one walk
(`deriveLine()`) after every edit. A per-line term is a field of `LineState`
(`softEarly`) and a running total (`totalSoftEarly_`), in five functions: set to 0 and
summed in `deriveLine()`, set to 0 and summed in `rebuild()`, subtracted in
`before_list_edit()`, added in `after_list_edit()`, and weighted in `current_score()`.
The line in `deriveLine()`:

```cpp
        if (end < job.idealEndTime) state.softEarly += job.idealEndTime - end;
```

`fp_plugin_test` has a check per component against `ConstraintBreakdown` in
`verifyConstraintTotals()`; add the new member's.

The default path ran at 2.5 million steps per second with the sixth constraint in it (2.7
with five), and ended on the score the assembly gave at weight 1000. The solves differ,
so the two speeds are not the cost of the term alone: with the constraint on, lines are
longer.

**4. What else moves.** A constraint that is on by default changes every default score:

- ten checks failed here beside the catalog row of step 3 and
  `test_constraint_info_reports_the_cpp_defaults`: five hand-computed scores in
  `tests/food_packaging/cpp/fp_test.cpp`, three in `test_constraints.py` and two in
  `test_models.py`. Their jobs end before their ideal end times, most of which are set
  far in the future, and the expected scores did not count that;
- the tables and the "five" in [README.md](README.md), [python.md](python.md),
  [cpp.md](cpp.md), `constraints.py`, `__init__.py`, `_core.pyi`, `common.hpp` and
  `score_calculator.hpp`;
- the CP-SAT model the benchmark compares against, `external_solver.food_packaging` in
  [external-solver](https://github.com/optimaru/external-solver): a plugin that
  subclasses the Python spec and takes its defaults from `constraint_info()`. Without
  the same constraint there,
  [`benchmarks/food-packaging`](../../../benchmarks/food-packaging) no longer compares
  one objective;
- the published results in `benchmarks/food-packaging/README.md`, and `CHANGELOG.md`.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed:

```python
import time

import optimaru.food_packaging as fp
from optimaru.food_packaging.constraints import get_constraints

steps = 500_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    schedule = fp.generate_demo_data()  # solve() works in place, so every run gets a fresh plant
    config = fp.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    fp.solve(schedule, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e6:5.2f} M steps/s  {schedule.score}")
```

Both lines end on the same score: same seed, same steps, same scoring. If they differ
after a change, the two paths no longer score alike. That holds while no two lines
share an operator; when they do, the default path searches in several legs and
reassigns operators between them, and the assembly does not.

| Million steps per second | Demo plant: 100 jobs, 5 lines | `generate_demo_data(10, 300)`: 300 jobs, 10 lines |
| --- | --- | --- |
| default path, five constraints | 2.7 | 2.0 |
| assembly, five constraints | 0.26 | 0.030 |
| assembly, five + the constraint of step 2 at weight 0 | 0.26 | 0.030 |
| assembly, five + the constraint at weight 1000 | 0.24 | 0.027 |
| assembly, five constraints without `operator_cleaning_conflict_cost` (step 4) | 2.2 | 1.8 |
| default path, six constraints (step 5) | 2.5 | 1.8 |

Each is the median of three runs, of 500,000 steps on the demo plant and 200,000 on
the larger one. The second and third rows differ by noise only: a constraint at weight 0 is
asked on no move.

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
scores at the same budget over several seeds, before and after:

```python
for seed in range(1, 6):
    schedule = fp.generate_demo_data()
    fp.solve(schedule, time_limit_seconds=5, random_seed=seed, constraints=picks)
    print(seed, schedule.score)
```

This use case has no C++ quality bench. Pass `constraints=None` to compare the default
path after step 5, from two checkouts, each with its own build. Against CP-SAT:
`uv run food-packaging-compare --seconds 10 --workers 8` in
`benchmarks/food-packaging/cpsat`
([README](../../../benchmarks/food-packaging/README.md)).

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/fp/constraints/<subject>_cost.hpp` | new | | | weight |
| `cpp/include/fp/constraints.hpp` | include, `emplace` | | | |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | counts in the text |
| `tests/food_packaging/cpp/fp_plugin_test.cpp` | | cases, counts | | breakdown check, catalog row |
| `tests/food_packaging/test_constraint_specs.py` | | lists | | defaults |
| `docs/use-cases/food-packaging/README.md`, `python.md`, `cpp.md` | | table row | | defaults |
| `cpp/src/score_calculator.cpp`, `cpp/include/fp/score_calculator.hpp` | | | | the term |
| `cpp/src/incremental_calculator.cpp`, `cpp/include/fp/incremental_calculator.hpp` | | | | the term |
| `tests/food_packaging/cpp/fp_test.cpp`, `test_constraints.py`, `test_models.py` | | | | expected scores |
| external-solver, `benchmarks/food-packaging` | | | | the same constraint, results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

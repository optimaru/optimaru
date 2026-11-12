# Assembly-line balancing: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its ten constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | the raw total in the monolithic calculators, hand cases, the reference scorer, the lists in the tests |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | nothing for the example |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `lateWorkCost`, each task's duration times the index of its station. It
pulls the work towards the first stations and closes the empty stations a default solve leaves
inside the line. Every snippet below was compiled and run, and every figure measured, on optimaru
0.4.3, the steps per second on 0.5.1; a later change to the solver moves the figures, not the
steps.

## How a line is scored

A task is assigned a station. The score is kept per **unit**: one station, with the
tasks assigned to it as its **members**. A move changes one or two tasks (or, with
`CloseStationMove`, the tasks of one station), so only the stations they leave and
join are scored again.

There are two scoring paths, and they give the same score for the same line
(`alb_plugin_test` checks it on 500 random assignments, with every constraint's raw total):

| | `alb.solve(line)` | `alb.solve(line, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/alb/constraints/` |
| Constraints | the nine that are on by default | the ones you list, with your knobs |
| Steps per second | 10.5 million | 4.8 million for the same nine, 4.9 million for `get_constraints()` |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are a generated line of 100 tasks (the "large" line of the benchmark), 2
million steps, one thread of an Apple M4 Max; [step 6](#6-measure) has the script.
`get_constraints()` returns all ten constraints of the catalog, the opt-in
`workload_imbalance_cost` at weight 0 among them. A constraint at weight 0 costs a move
nothing, so `get_constraints()` runs at the speed of the nine; picked with a weight, that
one keeps an ordered set of the station loads and costs more than the nine together
([step 4](#4-make-it-cheaper-per-move)). Steps 1 to 4 work on the assembly. Step 5 moves a
constraint to the monolithic calculators.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import json
from pathlib import Path

import optimaru.assembly_line_balancing as alb
from optimaru.assembly_line_balancing.constraints import FixedTaskStationsCost, UsedStationsCost, get_constraints
from optimaru.assembly_line_balancing.models import AssemblyLineBalancingProblemModel

demo = json.loads(Path("tests/assembly_line_balancing/fixtures/web_demo.json").read_text())
line = AssemblyLineBalancingProblemModel.model_validate(demo)  # 12 tasks, cycle time 12
changed = {"used_stations_cost", "fixed_task_stations_cost", "different_station_pairs_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    UsedStationsCost(weight=10),                                       # heavier (default 1)
    FixedTaskStationsCost(level="soft"),                               # hard by default
]                                                                      # different_station_pairs_cost: dropped
result = alb.solve(line, time_limit_seconds=2, constraints=picks)      # a new, solved model
```

To try weights without solving again, score a finished line with other picks:

```python
from optimaru.assembly_line_balancing.constraints import score

raw = alb.constraint_costs(result, picks)  # raw cost of every constraint, per station
print(score(raw, picks))                   # the score those picks give this line
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/assembly_line_balancing/constraints.py).
Stop here if the constraint you need is one of the ten.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

Four of the ten constraints are a sum of one term per task, and
[`constraints/common.hpp`](../../../optimaru/assembly_line_balancing/cpp/include/alb/constraints/common.hpp)
has a base for that. Pick the base by what the constraint is:

| The constraint is | Base | You write |
| --- | --- | --- |
| a sum of one term per task, read off the task and its station | `PerTaskConstraint` | `taskCost(problem, station, task)` |
| a cost per station that is no such sum (a clamp, pairs of tasks) | `AlbUnitConstraint` | `cost(problem, station, members)` |
| one number across stations (arcs, groups, a count, a range) | `MemberChangeGlobalConstraint` | `reset()`, `on_member_changed()`, `cost()` |

The exponent, offset and clip of a pick shape each station's cost for the first two
kinds and the one total for the third.

The example is a term per task, so it is one function. Four edits:

**1. The component**, `optimaru/assembly_line_balancing/cpp/include/alb/constraints/late_work_cost.hpp`:

```cpp
#ifndef ALB_CONSTRAINTS_LATE_WORK_COST_HPP
#define ALB_CONSTRAINTS_LATE_WORK_COST_HPP

#include "alb/constraints/common.hpp"

namespace alb {

// Soft, off by default (weight 0): lateWorkCost — the work a line does late.
// A task on station s costs duration * s (stations count from 0, so work on
// the first station is free). Raw cost per station = s times the processing
// time of its tasks; 0 = every task on station 0. Minimizing it moves work
// towards the first stations, so the idle time ends up at the end of the line.
struct LateWorkCost final : PerTaskConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit LateWorkCost(double weight = kDefaultWeight)
        : PerTaskConstraint("lateWorkCost", weight, kDefaultLevel) {}
    double taskCost(const AssemblyLineBalancingProblem& problem, int station,
                    int task) const override {
        return static_cast<double>(problem.tasks[task].duration * station);
    }
};

}  // namespace alb

#endif  // ALB_CONSTRAINTS_LATE_WORK_COST_HPP
```

The name passed to the base, `"lateWorkCost"`, is the struct name with a lower-case
first letter. Python derives `late_work_cost` from it.

The defaults live in the two constants and nowhere else; Python reads them through
`alb.constraint_info()`. `kDefaultLevel` is `Hard` or `Soft`: this use case has no
medium level (its `Score` in `domain.hpp` is the engine's two-level score), and a
pick with `level="medium"` raises `ValueError`. A hard constraint returns whole numbers,
because a hard contribution is rounded per station.

**2. The assembly**, `cpp/include/alb/constraints.hpp`: include the header, and add
one line to `forEachDefaultConstraint()`, before `WorkloadImbalanceCost`:

```cpp
    visit(std::make_unique<LateWorkCost>());
```

That function states the catalog order once, for `list_constraints()` and
`constraint_info()`. The docs and the tests select defaults by name, not by position
(`spec.name == "workload_imbalance_cost"`), so the new entry displaces none of their picks.

A constraint has to be in that list for Python to see it. With `kDefaultWeight = 0.0` it
is there and scores nothing until a caller gives it a weight, so no default solve
changes.

**3. The Python spec**, `optimaru/assembly_line_balancing/constraints.py`. A test
looks for the word `Default:` in the docstring:

```python
class LateWorkCost(Constraint):
    """Work placed late on the line: task duration times station index. Default: soft, weight 0 (opt-in)."""

    name = "late_work_cost"
    label_en = "Late work"
    label_ko = "뒤쪽 작업장 작업량"
    description_en = (
        "For each task: its duration times the index of its station, counted from 0. "
        "0 means every task is on the first station; a lower value means more of the work is done early on the line."
    )
    description_ko = (
        "작업마다 작업 시간에 배정된 작업장 번호(0부터 셉니다)를 곱해 더합니다. "
        "0이면 모든 작업이 첫 작업장에 있으며, 값이 작을수록 더 많은 작업이 라인 앞쪽에서 이루어집니다."
    )
```

**4. Its default instance**, in the same file: `late_work_cost = LateWorkCost()`
with the other instances, that instance in `_DEFAULTS` before
`workload_imbalance_cost` (the catalog order of edit 2), and both names in `__all__`, which
stays sorted (`ruff check` refuses it otherwise).

Build and use it. The web demo is too small to show the constraint (four seeded default
solves of five already end on the total the constraint asks for), so this is the 100-task
line of the benchmark:

```sh
export OPTIMARU_USE_CASES=assembly_line_balancing
uv sync                                    # rebuilds optimaru.assembly_line_balancing._core
```

```python
import random

from optimaru.assembly_line_balancing.constraints import LateWorkCost


def generated_line(task_count=100, cycle_time=120, seed=2):
    """The "large" line of benchmarks/assembly-line-balancing: a layered precedence graph, no zoning."""
    rng = random.Random(seed)
    tasks = [
        alb.Task(f"T{index + 1:04d}", rng.randint(20, 60) if rng.random() < 0.2 else rng.randint(1, 30))
        for index in range(task_count)
    ]
    arcs = [
        alb.Precedence(rng.randrange(max(0, task - 12), task), task)
        for task in range(1, task_count)
        for _ in range(rng.choice([0, 1, 1, 2, 3]))
    ]
    return alb.AssemblyLineBalancingProblem(cycle_time, tasks, arcs)


others = [spec for spec in get_constraints() if spec.name != "late_work_cost"]
picks = [*others, LateWorkCost(weight=0.01)]  # a small weight: the station count stays the first concern
config = alb.SolverConfig()
config.step_count_limit = 2_000_000
config.random_seed = 1
line = generated_line()
alb.solve(line, config=config, constraints=picks)  # a compiled problem is solved in place
late = sum(alb.constraint_costs(line, picks)["late_work_cost"]["costs"])
print(late, line.stations_used(), len(line.station_loads()))
```

On that line the sum is 15,146, on 17 stations with no empty one among them. A
default solve of the same length (`constraints=None`) leaves it at 19,735: 17
stations too, spread over the first 22.

What the base gave you: `cost()` of a station is the sum of `taskCost` over its
tasks, and when a task moves the engine calls `taskCost` once for the station it
leaves and once for the station it joins. The constraint is already incremental.

## 3. Check it

**The raw total in the monolithic calculators.** Here they report the raw total of
every constraint of the catalog, on or off, in `RawCosts`
(`cpp/include/alb/score_calculator.hpp`). `alb_plugin_test` compares the assembly with
it name by name, and `alb_test` the incremental calculator with the full recompute. So
an opt-in constraint gets its total there too, unweighted: a field `lateWorkCost` in
`RawCosts`, one line in the loop over the tasks of `ScoreCalculator::rawCosts()`,

```cpp
        raw.lateWorkCost += task.duration * s;
```

and, in `rawCosts()` of `cpp/src/incremental_calculator.cpp`, a scan. The default
path keeps nothing per move for a constraint it does not score (`workloadImbalanceCost`,
the other opt-in constraint, is read from the station loads in the same function):

```cpp
    // lateWorkCost is opt-in (default weight 0) and not in the score: it is
    // not kept per move, only read here for the tests (an O(tasks) scan).
    for (std::size_t task = 0; task < station_.size(); ++task) {
        if (station_[task] >= 0)
            raw.lateWorkCost += problem_->tasks[task].duration * station_[task];
    }
```

**Cases worked out by hand.** The two fixtures of
`tests/assembly_line_balancing/cpp/alb_fixtures.hpp` carry every constraint's total in their
comments; add the new one (23 on fixture H, 5 on fixture Z). Then, in
`alb_plugin_test.cpp`, next to the other raw costs of `verifyRawCostsByHand()` and
the other overrides of `verifyOverrides()`:

```cpp
    check(unit_costs(fixtureH(), "lateWorkCost") == V({0, 5, 0, 18}),
          "late work per station: [0, (3 + 2) * 1, 0, 6 * 3]");
```

```cpp
    AssemblyLineBalancingProblem front = fixtureZ();
    for (Task& t : front.tasks) t.station = 0;
    check(unit_costs(front, "lateWorkCost") == V({0, 0, 0, 0}),
          "late work zero: every task on station 0");
```

```cpp
    check(sameScore(scoreWith(defaultConstraints().weight("lateWorkCost", 2), fixtureH()),
                    Score{-10, -49.0}),
          "late work weight 2: -3 - 2 * 23 = -49soft");
```

`alb_test.cpp` checks the monolithic side by hand; add the same three totals to
`verifyConstraints()` (`h.lateWorkCost == 23`, 5 on fixture Z, 0 with no task
assigned).

```sh
cmake -B build -DOPTIMARU_USE_CASES=assembly_line_balancing && cmake --build build -j
ctest --test-dir build --output-on-failure        # alb_test, alb_plugin_test, the engine tests
uv run pytest -q tests/assembly_line_balancing tests/test_constraints_contract.py
```

**The counts.** After step 2 alone, an eleventh constraint fails three checks of
`alb_plugin_test` and 142 Python tests: 7 that count or list the catalog, and 135 of
`test_reference_parity.py` (the reference scorer, below). Update them:

- `alb_fixtures.hpp`: `kNames` and `namedTotals()`, the names and the `RawCosts`
  fields the two C++ tests compare by;
- `alb_plugin_test.cpp`: `rows.size() == 10` in `verifyCatalog()`, with the new row
  as `rows[9]` (`rowIs<LateWorkCost>(rows[9], "lateWorkCost", L::Soft, 0, false)`),
  and the loop over fixture Z in `verifyRawCostsByHand()`, which expects 0 of every
  constraint but the station count;
- `tests/assembly_line_balancing/test_constraint_specs.py`: `NAMES`, `INSTANCES`, the
  rows of `test_constraint_info_reports_the_cpp_defaults`, the weights of
  `test_weight_none_means_the_component_default`, and the costs of
  `test_constraint_costs_match_every_constraint_by_hand`;
- `test_assembly_line_balancing.py`: the totals of
  `test_zoning_and_station_domain_costs_match_manual_values`.

**The reference scorer.** `tests/assembly_line_balancing/reference_assembly_line_balancing.py`
is a second implementation in plain Python, and `test_reference_parity.py` compares
every constraint's total with it on samples and random lines. Add the constraint there,
written independently: its name in `REFERENCE_NAMES` and `ENGINE_NAME`, `(False, 0.0)` in
`REFERENCE_DEFAULTS`, its list in `constraint_costs()`, and its total in
`test_fixture_h_matches_the_hand_calculation`. The reference scores unassigned tasks too,
so count a task at station -1 as 0 work: `duration * max(0, station)`. With a plain
`duration * station`, `test_the_unassigned_web_demo_scores_zero` fails once the
constraint has a weight (step 5).

**Incremental against full.** Solve with every score checked against a full recompute. It raises
`RuntimeError` ("Score corruption: ...", the engine's `ScoreMismatchError`) (a `RuntimeError` in
Python) if a move leaves the score stale:

```python
alb.solve(generated_line(), time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

`alb_plugin_test` does the same for the default assembly, which now holds the constraint:
3,000 random moves, every constraint's raw total against a fresh assembly after each.

**The tables.** The constraint table in [README.md](README.md) must equal
`alb.constraint_info()`; add the row. The catalog is also written out in prose and
comments, and a new constraint makes them stale: the lists in `_core.pyi`, in the
docstring of `constraints.py` and above `forEachDefaultConstraint()` in `constraints.hpp`,
"The ten assembly-line-balancing constraints" and "the four per-task terms" in
`constraints/common.hpp`, "the six per-station constraints" and the header list of the
C++ section in README.md, the same count in `benchmarks/assembly-line-balancing/README.md`,
and the opt-in objective named in [python.md](python.md). The notebook
`examples/assembly-line-balancing.ipynb` prints the constraint list.

## 4. Make it cheaper per move

On the assembly a move costs, for every constraint, one call for the station a task leaves
and one for the station it joins. What that call is depends on the constraint:

| The constraint is | Use | Per move | Example to copy |
| --- | --- | --- | --- |
| a sum of per-task terms | `PerTaskConstraint` | one `taskCost` call | step 2 |
| a sum over pairs of tasks on one station | `AlbUnitConstraint` with `cost()`, `supports_delta()` and `delta_cost()` | the pairs of the moved task | [`different_station_pairs_cost.hpp`](../../../optimaru/assembly_line_balancing/cpp/include/alb/constraints/different_station_pairs_cost.hpp) |
| read off the whole station (a clamp) | `AlbUnitConstraint` with `cost()` | one pass over the tasks of the station | [`station_capacity_cost.hpp`](../../../optimaru/assembly_line_balancing/cpp/include/alb/constraints/station_capacity_cost.hpp) |
| anything that spans stations | `MemberChangeGlobalConstraint`: keep your own totals in `on_member_changed()` | what your update costs | [`used_stations_cost.hpp`](../../../optimaru/assembly_line_balancing/cpp/include/alb/constraints/used_stations_cost.hpp), [`workload_imbalance_cost.hpp`](../../../optimaru/assembly_line_balancing/cpp/include/alb/constraints/workload_imbalance_cost.hpp) |

A unit constraint with `cost()` alone is correct: the engine then recomputes the
whole station on every change. `delta_cost()` returns what one task adds given the
others, and must equal `cost(with the task) - cost(without it)` exactly. `full_assert`
(step 3) catches a delta that moves the score. One that is off by the same amount on
the station a task leaves and on the station it joins cancels in the total and shows
only when the pick has an `exponent` other than 1, so check a new delta with
`exponent=2` on the pick as well.

The example is already cheap: `PerTaskConstraint` answers the delta with one `taskCost`
call, and `taskCost` is one multiplication. There is nothing to change, so this step
changes no code. Measured on the nine default constraints, with one constraint added at
weight 0.01 (the weight of the solve of step 2) or at weight 0:

| Assembly | Steps per second |
| --- | --- |
| the nine default constraints | 4.81 million |
| + `lateWorkCost` as in step 2 | 3.80 million |
| + `lateWorkCost` written on `AlbUnitConstraint` with `cost()` alone | 4.04 million |
| + `workloadImbalanceCost` | 1.35 million |
| + `lateWorkCost` at weight 0 | 4.84 million |
| + `workloadImbalanceCost` at weight 0 | 4.88 million |

At weight 0.01 the second and third line end on different scores (-168.46 and -168.88
soft), so they take other moves, and most of the gap between them is that. At a weight
that every sum holds exactly, 2^-7, the two make the same moves and end on the same
score: five alternating rounds gave 4.10 million with the delta and 4.15 million with
`cost()` alone. A station holds six tasks here, so the pass of `cost()` is short, and on
this line the delta buys nothing. The fourth line is what a global with a costly update
does to every move: that constraint erases and inserts in an ordered set per change. The
last two lines are the speed of the nine: at weight 0 neither constraint is asked on a
move. Three more things cut the cost of a constraint.

**A constraint at weight 0 costs a move nothing.** The assembly asks a unit constraint
at weight 0 only when a solve starts and when `constraint_costs()` reports it, so
`get_constraints()`, which holds the opt-in constraints at weight 0, runs at the speed of
the constraints that are on. A global at weight 0 is still sent every change, because a
global may keep something besides its own cost. `workloadImbalanceCost` declares that it
does not:

```cpp
    bool idle_at_weight_zero() const override { return true; }
```

A global may declare it when nothing but its own `cost()` reads what it keeps and
`reset()` rebuilds all of it. At weight 0 it is then sent no change, and it is reset
before `constraint_costs()` reports its raw cost. The example is a unit constraint and
needs nothing.

**Say which units the constraint can charge.** `scoped_units()` returns the stations the constraint
is ever non-zero for, and the engine skips it on the others.
[`max_stations_cost.hpp`](../../../optimaru/assembly_line_balancing/cpp/include/alb/constraints/max_stations_cost.hpp)
builds that list in `on_reset()`, which runs when a solve starts: the stations from `max_stations`
on. A pick with `clip=False` then charges those stations only, and `constraint_costs()` lists them
as `units`.

**Read prepared lookups.** `ScoreCalculator::prepare()` derives what several constraints
and the monolithic calculators read (`arcsOut`, `pairsOf`, `forbiddenSorted`, ...)
before the search starts, not per move. A term that searches a fact list belongs there
or in `on_reset()`.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`alb.solve(line)` must score it, and `alb.solve(line)` scores with the monolithic
calculators: more than twice the steps per second of the assembly, and the only path
that runs parallel starts.

`alb_plugin_test` compares the default assembly with the monolithic calculators on
random assignments, so it fails from the moment the weight is not 0 until both sides
agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 1.0;`.
It must be a whole number, because the monolithic calculators add in `int64`.

**2. The term.** Since step 3 the calculators report the raw total. What is left is
to weigh it, and to keep it per move.

`cpp/src/score_calculator.cpp`: include the header, add the constraint to the two
`static_assert`s (a whole weight, the soft level), and add
`weightOf<LateWorkCost>() * raw.lateWorkCost` to the soft sum of
`ScoreCalculator::score()`.

`cpp/src/incremental_calculator.cpp` and its header: include the component's header,
and add a total `lateWork_`, set to 0 in `rebuild()`, returned by `rawCosts()` in place
of the scan of step 3, weighed in `score()`, and kept in `addTaskTerms()`, which the
calculator calls once for the station a task leaves and once for the station it joins:

```cpp
    lateWork_ += sign * t.duration * station;
```

That is all for a per-task constraint. The default path ran at 9.3 million steps per
second with the tenth constraint on, against 10.5 million with nine; the two searches take
different moves, so the difference is not all scoring.

A constraint over arcs, pairs or groups is more work: `taskPrecedenceCost` and
`differentStationPairsCost` are kept from the mirrored stations of the other tasks in
`reversedArcsOf()` and `pairsWith()`, `sameStationGroupsCost` from a tally per group in
`updateGroups()`, and a new constraint of that kind needs its own function next to them
in `insert()` and `retract()`.

**3. The reference scorer.** Its default for the constraint becomes `(False, 1.0)`; left
at 0, its default score differs from the engine's and 102 more tests of
`test_reference_parity.py` fail. With that, the assembly, the two monolithic calculators
and the reference agree on every score and raw total again; what still fails is
hand-computed (below).

**4. What else moves.** A constraint that is on by default changes every default score:

- hand-computed scores: 3 checks of `tests/assembly_line_balancing/cpp/alb_test.cpp`
  (the default scores of the fixtures and of the construction), 7 of
  `alb_plugin_test.cpp` (the partial assemblies and the overrides) and its catalog
  row, and 9 Python tests that state the defaults or score or solve at them
  (`test_assembly_line_balancing.py`, `test_constraint_specs.py`,
  `test_reference_parity.py`, `test_solve.py`);
- the solutions themselves. At weight 1 a unit of late work counts as much as a
  station. On the 100-task line four seeds of five still ended on 17 stations and
  one on 20 (2 million steps). A default that must never cost a station weighs
  `usedStationsCost` up with it: with that constraint at 1,000, seeds 1 to 5 all kept 17;
- the defaults and the score formula (`soft = -(stations used)`) wherever they are
  written out: [README.md](README.md), [python.md](python.md), `constraints.py`,
  `__init__.py`, `_core.pyi`, `domain.hpp`, `score_calculator.hpp`,
  `incremental_calculator.hpp` and `constraints.hpp`;
- the CP-SAT model the benchmark compares against,
  `external_solver.assembly_line_balancing` in
  [external-solver](https://github.com/optimaru/external-solver): a plugin that
  subclasses the spec and takes its defaults from `constraint_info()`, with an exact
  encoding, and its place in `REGISTRY`. That repository's
  `tests/assembly_line_balancing/test_constraint_parity.py` pins assignments in
  CP-SAT and compares every name of `list_constraints()` with the engine's raw
  cost, and the model refuses a spec it has no plugin for (`unknown constraint`), so
  that test asks for the plugin from step 2 on. Without the plugin
  [`benchmarks/assembly-line-balancing`](../../../benchmarks/assembly-line-balancing)
  still runs, but CP-SAT minimizes the objective without the constraint while both
  answers are scored with it: it no longer compares one objective;
- the published results in `benchmarks/assembly-line-balancing/README.md` and
  `cpsat/PUBLIC_BENCHMARKS.md`, and `CHANGELOG.md`.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed, on the
line of step 2 (`generated_line()` is the function defined there):

```python
import time

catalog = {row.name: row for row in alb.constraint_info()}
enabled = [spec for spec in get_constraints() if catalog[spec.name].default_enabled]
steps = 2_000_000
for label, picked in (("default path", None), ("assembly", enabled), ("get_constraints()", get_constraints())):
    line = generated_line()
    config = alb.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    alb.solve(line, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:18s} {steps / seconds / 1e6:5.2f} M steps/s  {line.score}")
```

All three lines end on the same score: same seed, same steps, same scoring. If they
differ after a change, the two paths no longer score alike.

| Measured on the 100-task line, 2 million steps | Steps per second |
| --- | --- |
| default path, nine constraints | 10.5 million |
| assembly, the same nine | 4.8 million |
| assembly, `get_constraints()` (the workload constraint at weight 0 with them) | 4.9 million |
| assembly, nine + the constraint of step 2 at weight 0.01 | 3.8 million |
| default path, ten constraints (step 5) | 9.3 million |
| assembly, the same ten (step 5) | 3.9 million |

Each figure is the median of three runs, all taken in one session; a run moves by a few
percent, and other work on the machine lowers every row together. A constraint with a
weight changes which moves are taken, so the last three lines are other searches than
the first three. The third line is the search of the second: a constraint at weight 0
judges no move differently and costs a move nothing.

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
the result at the same budget over several seeds, before and after:

```sh
cd benchmarks/assembly-line-balancing/cpsat
export OPTIMARU_USE_CASES=assembly_line_balancing   # without it, uv run rebuilds every use case
uv sync
uv run python -m assembly_line_balancing_cpsat.benchmark --solver engine --seconds 10 --seed 1
uv run python -m assembly_line_balancing_cpsat.benchmark --seconds 10      # against CP-SAT
```

Both commands solve on the default path, so they measure a constraint after step 5, on
the "small", "medium" and "large" generated lines of the benchmark. They print the score
at the default constraints: the station count before step 5, stations plus late work
after it. To compare stations across the change, read `stations_used()` as the snippet
of step 2 does. The public archive runs through `uv run assembly-line-balancing-public-bench`
([README](../../../benchmarks/assembly-line-balancing/README.md)), which reports the
stations used.

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/alb/constraints/<subject>_cost.hpp` | new | | | weight |
| `cpp/include/alb/constraints.hpp` | include, `visit` | the list in its comment | | the default in that list |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | lists in the docstring | | defaults in the docstrings |
| `cpp/include/alb/score_calculator.hpp`, `cpp/src/score_calculator.cpp` | | `RawCosts`, the raw total | | the weight in `score()` |
| `cpp/src/incremental_calculator.cpp`, its header | | the raw total, scanned | | kept per move, weighed |
| `tests/assembly_line_balancing/cpp/alb_fixtures.hpp` | | names, totals | | |
| `tests/assembly_line_balancing/cpp/alb_plugin_test.cpp`, `alb_test.cpp` | | cases, counts | | expected scores |
| `tests/assembly_line_balancing/test_constraint_specs.py`, `test_assembly_line_balancing.py` | | lists | | expected scores |
| `tests/assembly_line_balancing/test_solve.py` | | | | expected scores |
| `tests/assembly_line_balancing/reference_assembly_line_balancing.py`, `test_reference_parity.py` | | the constraint, weight 0 | | weight, expected score |
| `cpp/include/alb/constraints/common.hpp`, `_core.pyi` | | counts and lists in the comments | | |
| `__init__.py`, `_core.pyi`, `cpp/include/alb/domain.hpp` | | | | the score formula |
| `docs/use-cases/assembly-line-balancing/README.md`, `python.md` | | table row, counts | | defaults |
| external-solver, `benchmarks/assembly-line-balancing` | | the count in its README | | the same constraint, results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

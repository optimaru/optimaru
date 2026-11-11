# Task assigning: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its six constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | two C++ test cases, six checks that count or list the catalog |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the one global constraint, one C++ test case |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | both monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `customerSwitchCost`, the times an employee goes from a task of one
customer to a task of another. It keeps an employee's work for one customer together. Every snippet
below was compiled and run, and every figure measured, on optimaru 0.4.3; a later change to the
solver moves the figures, not the steps.

## How a solution is scored

Every employee has an ordered list of tasks, done back to back; a task on no list is
unassigned. The score is kept per **unit**: one employee, with the tasks on its list as
its **members**, in execution order. A move changes one or two lists, and only those
are scored again, each as a whole.

There are two scoring paths, and they give the same score for the same solution
(`ta_plugin_test` checks it on 300 random list layouts):

| | `ta.solve(solution)` | `ta.solve(solution, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/ta/constraints/` |
| Constraints | the six, at their defaults | the ones you list, with your knobs |
| Steps per second | 7.8 million | 2.1 million |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are the demo instance (28 tasks, 8 employees), 2 million steps, one thread
of an Apple M4 Max; [step 6](#6-measure) has the script and a larger instance. Steps 1
to 4 work on the assembly. Step 5 moves a constraint to the monolithic calculators.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import optimaru.task_assigning as ta
from optimaru.task_assigning.constraints import CriticalPriorityTaskEndTimeCost, UnassignedTasksCost, get_constraints
from optimaru.task_assigning.demo import generate_demo_data

solution = generate_demo_data()
changed = {"critical_priority_task_end_time_cost", "unassigned_tasks_cost", "minor_priority_task_end_time_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    CriticalPriorityTaskEndTimeCost(weight=8),                          # heavier (default 4)
    UnassignedTasksCost(level="hard"),                                  # medium by default
]                                                                       # minor_priority_task_end_time_cost: dropped
result = ta.solve(solution, time_limit_seconds=10, constraints=picks)
```

To try weights without solving again, score a finished solution with other picks:

```python
from optimaru.task_assigning.constraints import score

raw = ta.constraint_costs(result.to_core(), picks)  # raw cost of every constraint, per employee
print(score(raw, picks))                            # the score those picks give this solution
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/task_assigning/constraints.py).
Stop here if the constraint you need is one of the six.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

[`constraints/common.hpp`](../../../optimaru/task_assigning/cpp/include/ta/constraints/common.hpp)
has no base that asks for less than the engine's own. A constraint over one employee is a
`TaUnitConstraint` (the engine's `UnitConstraint`), and the one function you write is
`cost(solution, unit, members)`. What the file gives you to build on:

| The constraint | Build on | You write | Example to copy |
| --- | --- | --- | --- |
| reads the tasks of one employee, in order | `TaUnitConstraint` | `cost()` | [`required_skill_cost.hpp`](../../../optimaru/task_assigning/cpp/include/ta/constraints/required_skill_cost.hpp) |
| needs start or end times | the same, and `detail::walkChain()` | `cost()` with a callback per task end | [`squared_makespan_cost.hpp`](../../../optimaru/task_assigning/cpp/include/ta/constraints/squared_makespan_cost.hpp) |
| sums the end times of one priority | `PriorityTaskEndTimeCostBase` | a constructor that names the priority | [`critical_priority_task_end_time_cost.hpp`](../../../optimaru/task_assigning/cpp/include/ta/constraints/critical_priority_task_end_time_cost.hpp) |
| spans employees, or counts the tasks on no list | `TaGlobalConstraint` (the engine's `GlobalConstraint`) | `reset()`, `on_unit_changed()`, `cost()`, and `preview_cost()` ([step 4](#4-make-it-cheaper-per-move)) | [`unassigned_tasks_cost.hpp`](../../../optimaru/task_assigning/cpp/include/ta/constraints/unassigned_tasks_cost.hpp) |

The example reads one list in order. Four edits:

**1. The component**, `optimaru/task_assigning/cpp/include/ta/constraints/customer_switch_cost.hpp`:

```cpp
#ifndef TA_CONSTRAINTS_CUSTOMER_SWITCH_COST_HPP
#define TA_CONSTRAINTS_CUSTOMER_SWITCH_COST_HPP

#include "ta/constraints/common.hpp"

namespace ta {

// Soft, off by default (weight 0): customerSwitchCost — the times an employee
// goes from a task of one customer to a task of another. Raw cost per
// employee = the number of consecutive task pairs on its list whose customers
// differ, so 0 = all of the employee's tasks are for one customer. It keeps
// an employee's work for one customer together.
struct CustomerSwitchCost final : TaUnitConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit CustomerSwitchCost(double weight = kDefaultWeight)
        : TaUnitConstraint("customerSwitchCost", weight, kDefaultLevel) {}
    double cost(const TaskAssigningSolution& solution, int /*unit*/,
                const std::vector<int>& members) const override {
        std::int64_t switches = 0;
        for (std::size_t i = 1; i < members.size(); ++i) {
            if (solution.tasks[members[i]].customer != solution.tasks[members[i - 1]].customer)
                ++switches;
        }
        return static_cast<double>(switches);
    }
};

}  // namespace ta

#endif  // TA_CONSTRAINTS_CUSTOMER_SWITCH_COST_HPP
```

The name passed to the base, `"customerSwitchCost"`, is the struct name with a
lower-case first letter. Python derives `customer_switch_cost` from it.

The defaults live in these two constants and nowhere else; Python reads them through
`ta.constraint_info()`. `kDefaultLevel` is `Hard`, `Medium` or `Soft`, and the weight is
in units of that level. The score has four levels on those three (`domain.hpp`):

- hard is hard[0], and medium is soft[0]. Medium folds below hard (`kMediumFold`:
  `engine.hard = hard * 10^9 + medium`), so a medium constraint states its weight in medium
  units (`unassignedTasksCost`: 1) and no weight carries the 10^9. A hard or a medium
  constraint returns whole numbers, because both are rounded per employee.
- soft holds soft[1] and soft[2] together. `squaredMakespanCost` weighs 10^7 per squared
  minute there, which is what keeps it above the priority end times. A new soft constraint
  shares that level: with a weight far below 10^7 it decides between solutions of equal
  makespans, which here means the order of each employee's tasks, and its total shows in
  `soft2` (the soft level modulo 10^7).

**2. The assembly**, `cpp/include/ta/constraints.hpp`: include the header, and add
one line at the end of `defaultConstraints()`:

```cpp
    assembly.emplace<CustomerSwitchCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no default solve changes and the monolithic calculators stay as they are.

**3. The Python spec**, `optimaru/task_assigning/constraints.py`:

```python
class CustomerSwitchCost(Constraint):
    """Times an employee goes from one customer's task to another's. Opt-in: soft, weight 0."""

    name = "customer_switch_cost"
    label_en = "Customer switches"
    label_ko = "고객 전환"
    description_en = (
        "For each employee: the number of consecutive tasks on its list that are for different customers. "
        "One unit of cost is one switch; 0 means all tasks of every employee are for one customer."
    )
    description_ko = (
        "직원마다 목록에서 연달아 놓인 두 작업의 고객이 서로 다른 횟수를 셉니다. 고객이 바뀌는 1회가 비용 "
        "1이며, 0이면 모든 직원이 한 고객의 작업만 맡습니다."
    )
```

**4. Its default instance**, in the same file: `customer_switch_cost =
CustomerSwitchCost()` with the other instances, that instance in `_DEFAULTS`
before `unassigned_tasks_cost` (the per-employee constraints come first, the one global
last, as `list_constraints()` reports them), and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=task_assigning
uv sync                                    # rebuilds optimaru.task_assigning._core
```

```python
from optimaru.task_assigning.constraints import CustomerSwitchCost

others = [spec for spec in get_constraints() if spec.name != "customer_switch_cost"]
picks = [*others, CustomerSwitchCost(weight=200)]
config = ta.SolverConfig()
config.step_count_limit = 2_000_000
config.random_seed = 1
result = ta.solve(solution, config=config, constraints=picks)
print(sum(ta.constraint_costs(result.to_core(), picks)["customer_switch_cost"]["costs"]))
```

On the demo instance that sum is 7. A default solve of the same length
(`constraints=None`) leaves it at 12.

What the engine does with the one function: when a move changes an employee's list, it
calls `cost()` on the whole new list and replaces what that employee contributed.
`members` is the list in execution order, so a constraint can read neighbours, as this one
does. It must read nothing but `(solution, unit, members)`: `Task::employee` and
`Task::startTime` are filled in only when a solve ends, so a constraint that needs times
takes them from `detail::walkChain()`.

## 3. Check it

**Cases worked out by hand**, in `tests/task_assigning/cpp/ta_plugin_test.cpp`. One
next to the other single-component cases in `verifyPartialAssemblies()`:

```cpp
    {  // customerSwitchCost alone: four tasks for customers 0, 1, 1, 0 in a
        // row -> the employee changes customer twice; at weight 3 that is 6
        // on soft[2].
        Builder b;
        const int other = b.addCustomer();
        b.addEmployee({});
        const int first = b.addTask(10);
        const int second = b.addTask(10, {}, Priority::MINOR, /*minStart*/ 0, other);
        const int third = b.addTask(10, {}, Priority::MINOR, /*minStart*/ 0, other);
        const int fourth = b.addTask(10);
        b.solution.employees[0].tasks = {first, second, third, fourth};
        DecomposedCalculator calculator;
        calculator.emplace<CustomerSwitchCost>(3.0);
        checkScore("customerSwitchCost alone (3 * 2)", scoreWith(calculator, b.solution),
                   Score{0, 0, 0, -6});
    }
```

And, in `verifyRawCostsByHand()`, a violating and a clean case of the raw cost per
employee, as every constraint has there.

```sh
cmake -B build -DOPTIMARU_USE_CASES=task_assigning && cmake --build build -j
ctest --test-dir build --output-on-failure
```

**The counts.** A seventh constraint fails six checks that count or list the catalog.
Update them:

- `ta_plugin_test.cpp`: `constraint_totals().size() == 6` in `verifyConstraintTotals()`,
  and `rows.size() == 6` and the `names()` list in `verifyCatalog()`. Add the new row
  there, before `unassignedTasksCost`. `rowIs()` expects a constraint that is on by default,
  so check the new row's fields directly (`!rows[5].default_enabled`);
- `tests/task_assigning/test_constraint_specs.py`:
  `test_list_constraints_matches_the_spec_names` (the list),
  `test_module_level_instances_keep_every_default` (the instances) and
  `test_constraint_info_reports_the_cpp_defaults` (the rows).

**Incremental against full.** Solve with every score checked against a full recompute. It raises
`RuntimeError` ("Score corruption: ...", the engine's `ScoreMismatchError`) if a move leaves the
score stale:

```python
ta.solve(solution, time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

Run it once more with the constraint picked twice (`CustomerSwitchCost(weight=200),
CustomerSwitchCost(weight=1, exponent=2)`): the engine computes the raw cost of a
repeated pick once, and this checks that too. The 3,000 random moves of
`ta_plugin_test` run on the default assembly, where the constraint is at weight 0, so they
say nothing about its value.

**The tables.** The constraint tables in [README.md](README.md) and
[python.md](python.md) must equal `ta.constraint_info()`; add the row. The notebook
`examples/task-assigning.ipynb` prints the constraint list, and the docstrings of
`_core.pyi` name the constraints.

## 4. Make it cheaper per move

On the assembly a constraint pays one `cost()` call, over the whole list, each time the
engine edits a list. A move is several edits, and a rejected move is undone with as
many again: in 2 million steps on the demo instance the example's `cost()` ran 8.8
million times, 4.4 times a step. Two things cut that: what one call does, and how many
calls a move needs.

**What one call does.** Lists are short, so one plain pass over `members` is the right
shape for `cost()`, and a global constraint keeps its own totals per list in
`on_unit_changed()`; the table of step 2 names a file to copy for each kind of constraint.
The example compares one integer per task and has nothing left to cut. A term behind
strings or a lookup, such as the missing skills of an employee for a task type, belongs
in a table built in `on_reset()`. That is cheap here: `on_reset()` ran 3 times in the
2 million steps (at the start, when the best solution is put back at the end, and for
the final score). The engine also rebuilds after every rejected compound move, but this
use case switches none of them on. Under `full_assert` it rebuilds on every step.

**Let the assembly preview moves.** The engine can ask an assembly what a move would
score without applying it: it writes the candidate list out, calls `cost()` on it
once, and undoes nothing. It does so only when every global constraint that carries a
weight answers previews too. On `main` the one global here, `UnassignedTasksCost`,
does not, so the assembly applies, scores and undoes every move. These lines in
`cpp/include/ta/constraints/unassigned_tasks_cost.hpp` switch previews on for the
whole assembly:

```cpp
    // Preview contract: the count if each previewed list held its candidate
    // members. Without it the whole assembly would apply, score and undo
    // every move instead of previewing it.
    bool supports_preview() const override { return true; }
    double preview_cost(const TaskAssigningSolution& /*solution*/,
                        const optimaru::PreviewedUnit* changes, int count) const override {
        std::int64_t assigned = assigned_;
        for (int i = 0; i < count; ++i) {
            const std::int64_t size = static_cast<std::int64_t>(changes[i].members->size());
            assigned += size - sizes_[changes[i].unit];
        }
        return static_cast<double>(taskCount_ - assigned);
    }
```

With the constraint at weight 200 the assembly went from 2.0 to 3.9 million steps per second
and the example's `cost()` from 4.4 to 1.9 calls a step; the solve ended on the same
score, as it must. A constraint left at weight 0 is not asked during a preview at all: its
calls fell from 8.8 million to 0.24 million. This is done once for the use case, not
once per constraint. From then on `cost()` is called on lists that do not exist yet, which is
why it may read only `(solution, unit, members)`. A new case `verifyPreviews()` of
`ta_plugin_test.cpp` checks that with the engine's parity harness,
`optimaru::testing::run_preview_parity()`: 3,000 random moves on the default assembly
with the constraint at a weight, each preview against the score of the applied move.
`full_assert` (step 3) makes the same comparison during a solve. A new global
constraint must implement `supports_preview()` and `preview_cost()` too, or it
switches previews off again for every constraint.

**Answer a preview from sums.** A constraint can skip the written-out list and answer from
prefix sums it keeps per list (`supports_piecewise_preview()`, `piecewise_cost()`), as
vehicle routing does in
[`route_sums.hpp`](../../../optimaru/vehicle_routing/cpp/include/vrp/constraints/route_sums.hpp).
That pays on long lists. The demo instance has 3.5 tasks per employee, and no constraint
here does it.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`ta.solve(solution)` must score it, and `ta.solve(solution)` scores with the monolithic
calculators: 3.7 times the steps per second of the assembly on the demo instance (2
times after step 4), and the only path that runs parallel starts.

`ta_plugin_test` compares the default assembly with the monolithic calculators on
random layouts and random moves, so it fails from the moment the weight is not 0 until
both sides agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 200.0;`.
It must be a whole number, because the monolithic calculators add in `int64`.

**2. Where the calculators read it.** They never copy the number. Add
`customerSwitchWeight()` to `cpp/include/ta/constraints.hpp`, next to `priorityWeight()`
and written like it: it returns `CustomerSwitchCost::kDefaultWeight` as an `int64` and
has a `static_assert` that the weight is whole and the level soft.

**3. The term, twice.** `ScoreCalculator::score()` in `cpp/src/score_calculator.cpp` is
the full recompute. In its loop over an employee's tasks, with
`const Task* previous = nullptr;` declared before the loop:

```cpp
            // customerSwitchCost: a task for another customer than the one
            // before it. A soft term, so it joins soft[2].
            if (previous != nullptr && previous->customer != task.customer)
                weightedEndTimeTotal += customerSwitchWeight();
            previous = &task;
```

`insertList()` in `cpp/src/incremental_calculator.cpp` is what a move pays: it walks a
list once after every edit and keeps that list's totals. In that walk, with
`int previousCustomer = -1;` declared before the loop and `first = false;` moved to
its end:

```cpp
        // customerSwitchCost joins the list's soft[2] aggregate.
        if (!first && task.customer != previousCustomer) weightedEnd += customerSwitchWeight();
        previousCustomer = task.customer;
```

The level decides which total a term joins: `Score` has four fields, and a hard term
adds to `missingSkillTotal`, a medium one to `unassignedTotal` and a soft one to
`weightedEndTimeTotal`, which must stay below 10^7. A term behind strings gets a table
in `rebuild()` of the incremental calculator, next to `missingByEmployeeType_`.

The default path ran at 7.7 million steps per second with the seventh constraint in it (7.8
with six) and ended on the score the assembly gave at weight 200.

**4. What else moves.** A constraint that is on by default changes every default score:

- three checks failed here: the catalog row of step 3 in `ta_plugin_test.cpp` and
  `test_constraint_info_reports_the_cpp_defaults` (both say weight 0, off by default),
  and `test_weight_none_means_the_component_default`, whose list of explicit default
  weights has six constraints. `ta_test`, the hand-computed scores of the monolithic
  calculators, still passed: no case of it checks soft[2] on a list that mixes
  customers. Add a violating and a clean case there;
- the tables and the "six" in [README.md](README.md), [python.md](python.md),
  `constraints.py`, `__init__.py`, `_core.pyi`, `common.hpp` and `score_calculator.hpp`,
  and what `domain.hpp` says soft[2] holds;
- the CP-SAT model the benchmark compares against,
  [`external_solver.task_assigning.constraints`](https://github.com/optimaru/external-solver/blob/v0.3.0/src/external_solver/task_assigning/constraints.py)
  in external-solver: a plugin that subclasses the Python spec and takes its defaults from
  `constraint_info()`. Without the same constraint there,
  [`benchmarks/task-assigning`](../../../benchmarks/task-assigning) no longer compares one
  objective;
- the published results in `benchmarks/task-assigning/README.md`, and `CHANGELOG.md`.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed:

```python
import time

import optimaru.task_assigning as ta
from optimaru.task_assigning.constraints import get_constraints
from optimaru.task_assigning.demo import generate_demo_data

solution = generate_demo_data()
steps = 2_000_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    config = ta.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    result = ta.solve(solution, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e6:5.2f} M steps/s  soft {result.score_soft1} / {result.score_soft2}")
```

Both lines end on the same soft levels: same seed, same steps, same scoring. If they
differ after a change, the two paths no longer score alike.

| Million steps per second | Demo: 28 tasks, 8 employees | `xlarge`: 240 tasks, 32 employees |
| --- | --- | --- |
| default path, six constraints | 7.8 | 4.5 |
| assembly, six constraints | 2.1 | 0.84 |
| assembly, six + the constraint of step 2 at weight 0 | 2.0 | 0.80 |
| assembly, six + the constraint at weight 200 | 2.0 | 0.79 |
| assembly, six + the constraint at weight 200, with the previews of step 4 | 3.9 | 1.4 |
| default path, seven constraints (step 5) | 7.7 | 4.5 |

Each is the median of three runs of 2 million steps. `xlarge` is the largest instance
of [`benchmarks/task-assigning`](../../../benchmarks/task-assigning), 12 customers.

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
scores at the same budget over several seeds, before and after:

```python
for seed in range(1, 6):
    result = ta.solve(solution, time_limit_seconds=5, random_seed=seed, constraints=picks)
    print(seed, result.score_hard, result.score_soft0, result.score_soft1, result.score_soft2)
```

This use case has no C++ quality bench. Pass `constraints=None` to compare the default
path after step 5, from two checkouts, each with its own build. Against CP-SAT:
`uv run task-assigning-compare --seconds 10 --workers 8` in
`benchmarks/task-assigning/cpsat`
([README](../../../benchmarks/task-assigning/README.md)).

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/ta/constraints/<subject>_cost.hpp` | new | | | weight |
| `cpp/include/ta/constraints.hpp` | include, `emplace` | | | the weight for the calculators |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | counts in the text |
| `tests/task_assigning/cpp/ta_plugin_test.cpp` | | cases, counts | preview case | catalog row |
| `tests/task_assigning/test_constraint_specs.py` | | lists | | defaults |
| `docs/use-cases/task-assigning/README.md`, `python.md` | | table row | | defaults |
| `cpp/include/ta/constraints/unassigned_tasks_cost.hpp` | | | previews, once | |
| `cpp/src/score_calculator.cpp`, `cpp/src/incremental_calculator.cpp` | | | | the term |
| `tests/task_assigning/cpp/ta_test.cpp` | | | | new cases |
| external-solver, `benchmarks/task-assigning` | | | | the same constraint, results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

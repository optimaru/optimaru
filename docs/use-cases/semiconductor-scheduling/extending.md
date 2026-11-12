# Semiconductor fab scheduling: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its twenty constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-unit-formula) | Add a constraint, off by default | one header, a `Term`, four lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | two C++ test cases, the catalog in the tests, the Python reference scorer |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | nothing, for this constraint |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | one weight, one line in `ScoreCalculator::scoreOf` |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `lateLotsCost`, the number of lots that complete after their due time,
whatever the delay and the priority. `weightedTardinessCost` charges how late the lots are; this
constraint charges how many are late, the on-time delivery count a fab reports. Every snippet below
was compiled and run, and every figure measured, on optimaru 0.4.3; a later change to the solver
moves the figures, not the steps.

## How a schedule is scored

Each operation row (one per lot and route step) holds one decision value, a decode
priority and a machine choice folded into one int. The schedule is not stored: the
decoder ([`decoder.hpp`](../../../optimaru/semiconductor_scheduling/cpp/include/fab/decoder.hpp))
derives it from all the values, and changing one row's priority can move every later
operation on every machine. So no constraint is a sum over decision values, and all twenty
constraints are **global** components that read one shared view of the decoded schedule.

The view is the `ScheduleEvaluator`
([`evaluator.hpp`](../../../optimaru/semiconductor_scheduling/cpp/include/fab/evaluator.hpp)).
A move only marks its rows. When a score is asked, the evaluator decodes again from the
earliest step the marked rows can change, finds the operations whose machine, start or
end moved, and re-costs only the **units** they touch: their rows, their lots, the old
and new machines, those machines' groups, the resources they use and their pins. A
unit is re-costed by a static function of the constraint's own component, so each formula is
written once, in its header.

There are two scoring paths, and they give the same score for the same values
(`fab_plugin_test` checks it on 300 random states, in data and in search mode):

| | `fab.solve(problem)` | `fab.solve(problem, constraints=[...])` |
| --- | --- | --- |
| Scored by | `SemiconductorIncrementalScoreCalculator` (`cpp/src/incremental_calculator.cpp`): the evaluator's totals at the default weights of `ScoreCalculator::scoreOf` | the assembly: one component per constraint, `cpp/include/fab/constraints/`, each reading the same evaluator |
| Constraints | the twenty, at their defaults | the ones you list, with your knobs |
| Steps per second | 93.6 thousand | 91.1 thousand |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are lots24 of the benchmark (24 lots of four steps, 6 machines), 500,000
steps, one thread of an Apple M4 Max; [step 6](#6-measure) has the script. The two
paths run at the same speed because both pay for the same thing, the decode. Unlike
most use cases here, a constraint added in step 2 is computed on both paths from then on,
whatever its weight; step 5 only decides whether the default score counts it.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import optimaru.semiconductor_scheduling as fab
from optimaru.semiconductor_scheduling.constraints import MaxQueueTimeCost, WeightedTardinessCost, get_constraints
from optimaru.semiconductor_scheduling.models import SemiconductorScheduleModel

problem = SemiconductorScheduleModel.model_validate_json(open("tests/semiconductor_scheduling/fixtures/web_demo.json").read())
changed = {"weighted_tardiness_cost", "max_queue_time_cost", "machine_load_imbalance_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    WeightedTardinessCost(weight=40),                                  # heavier (default 10)
    MaxQueueTimeCost(level="soft"),                                    # hard by default
]                                                                      # machine_load_imbalance_cost: dropped
result = fab.solve(problem, time_limit_seconds=2, constraints=picks)
```

To try weights without solving again, score a finished schedule with other picks:

```python
from optimaru.semiconductor_scheduling.constraints import score

raw = fab.constraint_costs(result, picks)  # raw total of every constraint
print(score(raw, picks))                   # the score those picks give this schedule
print(fab.constraint_items(result)["weighted_tardiness_cost"])  # one item per completed lot
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/semiconductor_scheduling/constraints.py).
Every constraint is global, so `exponent` and `offset` shape its total, not each lot.
Stop here if the constraint you need is one of the twenty.

## 2. A new constraint from one unit formula

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are applied
by the engine.

All twenty derive from `ScheduleViewConstraint` in
[`constraints/common.hpp`](../../../optimaru/semiconductor_scheduling/cpp/include/fab/constraints/common.hpp).
The base binds the shared view, passes every value change to it and returns the view's total for the
component's `Term` from `cost()`. A component therefore adds no virtual function. It names its
`Term` and holds its formula as a static function over one unit; the evaluator and the full
recompute call that function. Pick the unit by what the constraint reads:

| The constraint reads | Static function | Example |
| --- | --- | --- |
| one operation row and its placement | `rowCost(schedule, row, p)` | `machine_preference_cost.hpp` |
| the route of one lot | `lotCost(schedule, lot, route, items)` | `cycle_time_cost.hpp` |
| the assignments of one machine, sorted | `machineCost(schedule, machine, rows, items)` | `missing_setup_time_cost.hpp` |
| the users of one auxiliary resource | `resourceCost(schedule, resource, rows, items)` | `auxiliary_resource_capacity_cost.hpp` |
| one pin | `pinCost(schedule, pin, actual)` | `pinned_assignments_cost.hpp` |
| the machine loads of one group | `machineLoad(rows)`, then `groupCost(loads, items)` | `machine_load_imbalance_cost.hpp` |
| the whole schedule | `scheduleCost(completions, items)` | `makespan_cost.hpp` |

The example reads the last step of one lot, so it is a `lotCost`. Six edits:

**1. The component**, `optimaru/semiconductor_scheduling/cpp/include/fab/constraints/late_lots_cost.hpp`:

```cpp
#ifndef FAB_CONSTRAINTS_LATE_LOTS_COST_HPP
#define FAB_CONSTRAINTS_LATE_LOTS_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab {

// Soft, off by default (weight 0): lateLotsCost — the number of late lots,
// whatever their delay and priority. Raw cost per lot with its last route step
// assigned = 1 when that step ends after the due time, else 0. 0 = every
// completed lot is on time.
struct LateLotsCost final : ScheduleViewConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;
    static constexpr Term kTerm = Term::LateLotsCost;

    explicit LateLotsCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("lateLotsCost", weight, kDefaultLevel, kTerm) {}

    static std::int64_t lotCost(const SemiconductorSchedule& schedule, int lot,
                                const ScorePlacement* const* route,
                                std::vector<std::int64_t>* items) {
        const Lot& l = schedule.lots[static_cast<std::size_t>(lot)];
        const ScorePlacement* last = route[l.operations.size() - 1];
        if (last == nullptr) return 0;
        return terms::emit(items, last->end > l.dueTime ? 1 : 0);
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_LATE_LOTS_COST_HPP
```

The name passed to the base, `"lateLotsCost"`, is the struct name with a lower-case
first letter. Python derives `late_lots_cost` from it. `route` holds one placement
per route step, `nullptr` where the decoder stopped the lot. `terms::emit()` appends
the lot's item to the per-item list when there is one (`fab.constraint_items()`):
here one item per completed lot, 0 or 1, the way `weightedTardinessCost` lists its
items.

The defaults live in these two constants and nowhere else; Python reads them through
`fab.constraint_info()`. `kDefaultLevel` is `Hard` or `Soft`: this domain has no
medium level, and `level="medium"` raises `ValueError`. Hard totals are rounded per
constraint, so a hard constraint returns whole numbers.

**2. The `Term`**, in
[`score_terms.hpp`](../../../optimaru/semiconductor_scheduling/cpp/include/fab/score_terms.hpp):
`LateLotsCost,` in `enum class Term` between `CycleTimeCost` and `MakespanCost`, and
`kTermCount = 21`. The `Term` order must equal the order of `defaultConstraints()`:
`constraint_items()` pairs the per-item lists with the catalog names by position.
The first `kHardTermCount` (14) terms are the hard ones, which `ScoreCalculator::scoreOf`
adds up by index; a soft constraint goes after them. The place before `MakespanCost` keeps
`get_constraints()[:-1]` meaning "all but the makespan", as the README and
`constraints.py` write it.

**3. The assembly**, `cpp/include/fab/constraints.hpp`: include the header, and add
one line to `defaultConstraints()`, after `CycleTimeCost`:

```cpp
    assembly.emplace<LateLotsCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight.

**4. The two callers of the formula.** The full recompute, `rawCostsOf()` in
`cpp/src/score_calculator.cpp`, in the loop over the lots:

```cpp
        add(Term::LateLotsCost,
            LateLotsCost::lotCost(schedule, lot, route, list(Term::LateLotsCost)));
```

The evaluator, `ScheduleEvaluator::recost()` in `cpp/src/evaluator.cpp`, in the loop
over the dirty lots, with a cache field `late` added to `LotCache` in `evaluator.hpp`:

```cpp
        move(Term::LateLotsCost, c.late,
             LateLotsCost::lotCost(schedule, lot, route.data(), nullptr));
```

`move()` takes the lot's old value out of the running total and puts the new one in.
`ScoreCalculator::scoreOf` stays as it is: it lists the soft terms it weighs, and the
new one is not among them, so no default score changes.

**5. The Python spec**, `optimaru/semiconductor_scheduling/constraints.py`. The
docstring has to contain `Default:`, a test checks it:

```python
class LateLotsCost(Constraint):
    """Number of late lots, whatever their delay. Default: soft, weight 0 (opt-in)."""

    name = "late_lots_cost"
    label_en = "Late lots"
    label_ko = "납기 지연 로트 수"
    description_en = (
        "The number of lots with a completed route whose completion passes their due time, whatever the delay "
        "and the priority. One unit is one late lot; 0 means every completed lot is on time."
    )
    description_ko = (
        "공정을 모두 마친 로트 가운데 완료 시각이 납기를 넘긴 로트의 수입니다. 지연 시간과 우선순위는 따지지 않습니다. "
        "지연 로트 1개가 비용 1이며, 0이면 공정을 마친 모든 로트가 납기를 지킵니다."
    )
```

**6. Its default instance**, in the same file: `late_lots_cost = LateLotsCost()` with
the other instances, that instance in `_DEFAULTS` before `makespan_cost`, and both
names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=semiconductor_scheduling
uv sync                                    # rebuilds optimaru.semiconductor_scheduling._core
```

```python
from optimaru.semiconductor_scheduling.constraints import LateLotsCost

problem = lots24()  # the benchmark instance, built by the script of step 6
others = [spec for spec in get_constraints() if spec.name != "late_lots_cost"]
picks = [*others, LateLotsCost(weight=50)]
config = fab.SolverConfig()
config.step_count_limit = 500_000
config.spent_limit_seconds = 600
config.random_seed = 1
result = fab.solve(problem, config=config, constraints=picks)
print(fab.constraint_costs(result, picks)["late_lots_cost"]["costs"], result.total_weighted_tardiness)
```

That prints `[4.0] 96`: four late lots. A default solve of the same length
(`constraints=None`) leaves 10 lots late at a weighted tardiness of 81. Keep the
time limit generous: the temperature follows the step fraction only while it leads
the time fraction, and only then does a seeded, step-limited solve repeat.

What the base gave you: every value change reaches the view, and the view re-costs a
lot only when one of its placements moved. The constraint is incremental from the start.

## 3. Check it

**Cases worked out by hand.** The positive case goes into `testHandCalculated()` in
`tests/semiconductor_scheduling/cpp/fab_test.cpp`, with the other checks on the fab
fixture (L1 is due at 12 and ends at 15; L2 and L3 are on time):

```cpp
    check(raw(fab, Term::LateLotsCost) == 1, "fab: L1 ends 15 past its due 12 -> 1 late lot");
```

The zero case is already there: the loop over the zero fixture checks every term but
cycle time and makespan for 0, and its lots are due at 50. In
`tests/semiconductor_scheduling/cpp/fab_plugin_test.cpp`, add `{"lateLotsCost", 1}` to
the map of `verifyRawCostsByHand()`, and a case that gives the constraint a weight to
`verifyOverrides()`:

```cpp
    {
        // lateLotsCost is opt-in: weight 0 by default, so it scores at a weight.
        DecomposedCalculator calculator;
        defaultConstraints().weight("lateLotsCost", 5).into(calculator);
        const Score score = scoreWith(calculator, s);
        check(score.hard == -26 && score.soft == -(135.0 + 5 * 1),
              "(d) lateLotsCost at weight 5: L1 is the one late lot, 5 soft");
    }
```

```sh
cmake -B build -DOPTIMARU_USE_CASES=semiconductor_scheduling && cmake --build build -j
ctest --test-dir build --output-on-failure
```

**The catalog.** A twenty-first constraint breaks these checks; update them:

- `kTermNames` in `tests/semiconductor_scheduling/cpp/fab_fixtures.hpp`, the registry
  names in `Term` order. It is sized `kTermCount`, so it still compiles with a name
  missing, and `fab_plugin_test` then crashes on the empty entry. Add `"lateLotsCost"`
  before `"makespanCost"`.
- `fab_plugin_test.cpp`: `verifyComponentsAlone()` assumed weight 1 for every constraint but
  weighted tardiness; read the weight from `defaultConstraints().info()` instead.
  `verifyCatalog()` counts 20 rows; make it 21 and check the new row's fields
  directly before `makespanCost` (`rowIs()` expects a constraint that is on by default).
- `testCatalog()` in `fab_test.cpp` counts 20 rows and expects every row on by
  default; make it 21 and `default_enabled == (default_weight > 0.0)`.
- `tests/semiconductor_scheduling/test_constraint_specs.py`: `NAMES`, the expected
  rows of `test_constraint_info_reports_the_cpp_defaults` (and its assertion that every
  row is on by default), and `test_weight_none_means_the_component_default`, which wrote
  out every default weight (read them from `fab.constraint_info()`).
- `tests/semiconductor_scheduling/reference_semiconductor_scheduling.py`, the
  independent scorer and decoder in plain Python that `test_reference_parity.py`
  compares the engine with: on the samples and 200 random instances, every raw total,
  every per-item list and the default score. It fails 414 cases until the constraint is in.
  Add `"late_lots"` to `REFERENCE_NAMES`, `"late_lots_cost"` to `ENGINE_NAMES` (both
  before the makespan), `"late_lots": (False, 0.0)` to `DEFAULTS`, and the items to
  `constraint_costs()`, in the loop over the lots:

  ```python
              late_lots.append(int(last.end > lot.due_time))
  ```

  with `late_lots = []` before the loop and `"late_lots": late_lots` in the dict it
  returns.

Then run the suites: `ctest` as above, and `pytest -q tests/semiconductor_scheduling
tests/test_constraints_contract.py`.

**Incremental against full.** Two checks, and they catch different mistakes:

- `fab_test` and `fab_plugin_test` (b) walk random moves and compare every term of the evaluator
  with `rawCostsOf()` of the decoded schedule. A formula missing from one of the two callers, or
  called differently, fails here. (b) also requires every term to be non-zero somewhere on the walk.
  - A solve with every score checked against a rebuild. It raises `RuntimeError` ("Score corruption:
  ...", the engine's `ScoreMismatchError`) if a move leaves a unit stale:

  ```python
  fab.solve(problem, time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
  ```

  The rebuild uses the same evaluator, so this cannot see a formula that is wrong or
  missing in it. With the `move()` line of edit 4 deleted, this solve passed, and both
  C++ walks failed.

The reference parity of the Python tests scores assignment lists as given, through
`rawCostsOf()`; it does not run the evaluator.

**The tables and the CP-SAT model.** The constraint table in [README.md](README.md)
and its "all 20 are scored by default" must agree with `fab.constraint_info()`; add
the row. "The 20 constraints" of the comments in `common.hpp` and `score_terms.hpp`
and "the 20 defaults" in `__init__.py` count the catalog. The benchmark compares against `external_solver.semiconductor_scheduling` in
[external-solver](https://github.com/optimaru/external-solver), one plugin per
constraint, a subclass of the Python spec; its catalog equals `constraint_info()`
([`cpsat/README.md`](../../../benchmarks/semiconductor-scheduling/cpsat/README.md)).
Add the plugin there, with a positive and a zero case in its
`tests/semiconductor_scheduling/test_parity.py`.

## 4. Make it cheaper per move

The example has nothing left to cut. Its formula is one comparison, run only for the
lots the move touched. What a step costs is the decode and the re-costing of the
touched units, and they are shared by every constraint. Three consequences differ from most
use cases here:

- The evaluator computes every `Term` at every score, on both paths, whether the constraint
  is picked or not and at any weight. A constraint left out of the picks still costs its
  formula. So a slow formula slows `fab.solve(problem)` too, from step 2 on.
- A pick at weight 0 is sent every change like the others. Elsewhere the engine leaves a unit
  constraint at weight 0 out of the moves, and a global one that declares
  `idle_at_weight_zero()`; no component here may declare it, because the view each one
  keeps up to date is the one all of them read.
- `cost()` of a component only reads a total. The work is in the static function and
  in how often the evaluator calls it.

So a constraint is cheap when its unit is small and its formula reads only that unit:

| The constraint reads | Re-costed when | Per call | Example to copy |
| --- | --- | --- | --- |
| a row | its placement moved | O(1) | [`machine_preference_cost.hpp`](../../../optimaru/semiconductor_scheduling/cpp/include/fab/constraints/machine_preference_cost.hpp) |
| a lot's route | any step of the lot moved | the lot's steps | [`max_queue_time_cost.hpp`](../../../optimaru/semiconductor_scheduling/cpp/include/fab/constraints/max_queue_time_cost.hpp) |
| a machine's sequence | an assignment joined, left or moved on it; the rows come sorted once for all machine constraints | the machine's assignments | [`missing_setup_time_cost.hpp`](../../../optimaru/semiconductor_scheduling/cpp/include/fab/constraints/missing_setup_time_cost.hpp) |
| a resource's users | one of them moved | a sweep over its users | [`auxiliary_resource_capacity_cost.hpp`](../../../optimaru/semiconductor_scheduling/cpp/include/fab/constraints/auxiliary_resource_capacity_cost.hpp) |
| a group of machines | one of its machines changed; each machine's load is cached in `MachineCache` | the group's machines | [`machine_load_imbalance_cost.hpp`](../../../optimaru/semiconductor_scheduling/cpp/include/fab/constraints/machine_load_imbalance_cost.hpp) |
| the whole schedule | any row moved; it reads the lots' cached completions | all lots | [`makespan_cost.hpp`](../../../optimaru/semiconductor_scheduling/cpp/include/fab/constraints/makespan_cost.hpp) |

A constraint that fits none of these units needs a new cache and dirty list in the
evaluator, filled in `markRowDirty()` or `markMachineDirty()`, and the same loop in
`rawCostsOf()`. Two more things keep a constraint on the cheap side.

**Read the lookups of `prepare()`.** The facts never change during a solve.
`ScoreCalculator::prepare()` builds the derived tables once, before a solve:
`setupByKey`, `transportByGroup`, `lastOptionOfMachine`, `pinOfRow`,
`rowsUsingResource`, the group and family indexes (`SemiconductorSchedule` in
[`domain.hpp`](../../../optimaru/semiconductor_scheduling/cpp/include/fab/domain.hpp)).
A constraint that needs another table adds it there, where `rawCostsOf()` and the evaluator
both read it. The components' `reset()` only resets the shared view, once per
rebuild of the calculator: at the start of a solve, and at every score under
`full_assert` (`Editor::score()` rebuilds to compare).

**Keep raw costs whole numbers, with `checked::` arithmetic.** Every raw cost is an
`int64`, and the evaluator moves totals by differences, which come back exactly only
in integers. Times and products can be large; `checked::add`, `sub` and `mul` in
`domain.hpp` throw `OverflowError` instead of wrapping.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`fab.solve(problem)` must count it, and it is the only path that runs parallel
starts. The default path already computes the term since step 2; what is missing is
its weight in `ScoreCalculator::scoreOf`. `fab_plugin_test` (a) compares the default
assembly with the monolithic score on random states, so it fails from the moment the
weight is not 0 until both agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 50.0;`.
The soft score is summed in `double`, so a soft weight need not be whole.

**2. The term**, in `cpp/src/score_calculator.cpp`. Read the weight off the component
with the other soft weights:

```cpp
constexpr double kLateLotsWeight = LateLotsCost::kDefaultWeight;
```

and add it to the list that `ScoreCalculator::scoreOf()` sums, in catalog order,
between cycle time and makespan:

```cpp
                                       std::pair{kLateLotsWeight, Term::LateLotsCost},
```

The order matters: the assembly adds its soft totals in catalog order, and the two
scores are compared bit for bit. `SemiconductorIncrementalScoreCalculator::score()`
calls `scoreOf()`, so the incremental path needs no change. The Python reference gets
the weight too: `"late_lots": (False, 50.0)` in `DEFAULTS`.

With that, both C++ parity checks and `test_reference_parity.py` pass. A constraint that is
hard by default is more work: `scoreOf()` adds the first `kHardTermCount` terms
unweighted, and a `static_assert` there requires every hard default weight to be 1.
Its `Term` goes into the hard block, `kHardTermCount` grows by one, and the
`static_assert` takes the new weight.

**3. What else moves.** A constraint that is on by default changes every default score.
These failed with the example at weight 50, and are expectations to update, not bugs:

- `fab_test`: "fab: soft = -135", the default score of the fab fixture, now -185
  (L1 is late);
- `fab_plugin_test`: three checks of `verifyOverrides()` that state soft scores at the
  defaults, and the new row in `verifyCatalog()` (weight 50, on by default);
- Python: `test_constraint_info_reports_the_cpp_defaults`. No other Python test failed;
- the default weights and the score formula in [README.md](README.md) (its table, and
  "all 20 are scored by default"), in [python.md](python.md), in the comments of
  `domain.hpp`, `score_calculator.hpp` and `constraints.hpp`, and in the docstrings of
  `constraints.py`, `__init__.py` and `_core.pyi`;
- the CP-SAT model: without the constraint at the same weight in
  `external_solver.semiconductor_scheduling`,
  [`benchmarks/semiconductor-scheduling`](../../../benchmarks/semiconductor-scheduling)
  no longer compares one objective. Its published results and `CHANGELOG.md` follow.

The default path ran at 94.7 thousand steps per second with the constraint in, as before.
Its seed-1 solve of step 6 ended on soft -2465, the score of the assembly with the
constraint picked at weight 50 in step 2: the same scoring, reached by the other path.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed, on
lots24 of the benchmark (`_build(24, seed=3)` in
`benchmarks/semiconductor-scheduling/cpsat/semiconductor_scheduling_cpsat/benchmark.py`):

```python
import random
import time

import optimaru.semiconductor_scheduling as fab
from optimaru.semiconductor_scheduling.constraints import get_constraints
from optimaru.semiconductor_scheduling.models import (
    LotModel,
    MachineModel,
    MachineOptionModel,
    OperationModel,
    SemiconductorScheduleModel,
)


def lots24() -> SemiconductorScheduleModel:
    """lots24 of benchmarks/semiconductor-scheduling: 24 lots of 4 steps on 6 machines."""
    rng = random.Random(3)
    families = ["PHOTO", "ETCH", "DIFF"]
    machines = [MachineModel(id=f"{family}{m}", group=family) for family in families for m in range(2)]
    lots = []
    for lot in range(24):
        operations = []
        for step in range(4):
            family = families[step % len(families)]
            options = [MachineOptionModel(machine_id=f"{family}{m}", duration=rng.randint(2, 8)) for m in range(2)]
            operations.append(OperationModel(id=f"S{step}", family=family, eligible_machines=options))
        lots.append(
            LotModel(
                id=f"L{lot}",
                product=f"P{lot % 3}",
                release_time=rng.randint(0, 10),
                due_time=rng.randint(40, 80),
                operations=operations,
            )
        )
    return SemiconductorScheduleModel(lots=lots, machines=machines)


problem = lots24()
steps = 500_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    config = fab.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    result = fab.solve(problem, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e3:6.1f} k steps/s  {result.score_hard} hard {result.score_soft} soft")
```

Both lines end on the same score: same seed, same steps, same scoring. If they
differ after a change, the two paths no longer score alike.

| Measured on lots24, 500,000 steps, median of three runs | Steps per second |
| --- | --- |
| default path, twenty constraints | 93.6 thousand |
| assembly, twenty constraints | 91.1 thousand |
| default path, the constraint of step 2 computed at weight 0 | 94.4 thousand |
| assembly, twenty + the constraint of step 2 at weight 0 | 89.7 thousand |
| assembly, twenty + the constraint of step 2 at weight 50 | 91.8 thousand |
| default path, twenty-one constraints (step 5) | 94.7 thousand |
| assembly, twenty-one constraints (step 5) | 91.0 thousand |

The differences between rows are within the spread of the runs (up to 3 thousand
steps per second between runs of one row): the constraint costs nothing measurable.

**What it buys.** A constraint changes what the search can reach, so compare the results at
the same budget over several seeds, with and without it:

```python
for seed in range(1, 6):
    config = fab.SolverConfig()
    config.step_count_limit = 500_000
    config.spent_limit_seconds = 600
    config.random_seed = seed
    for picked in (others, picks):
        result = fab.solve(problem, config=config, constraints=picked)
        late = fab.constraint_costs(result)["late_lots_cost"]["costs"][0]
        print(seed, late, result.total_weighted_tardiness, result.makespan)
```

`others` and `picks` are those of step 2. Without the constraint the five solves leave 10,
9, 9, 8 and 5 lots late; with it at weight 50, 4, 5, 4, 5 and 4, at a weighted
tardiness of 96 to 113 instead of 81 to 100. Fewer lots are late, and the ones that
are wait longer.

Against CP-SAT at a fixed time: `uv run semiconductor-scheduling-compare --seconds 10
--workers 8` in `benchmarks/semiconductor-scheduling/cpsat`
([README](../../../benchmarks/semiconductor-scheduling/README.md)). It solves on the
default path only, so it measures a constraint after step 5 and with the constraint in the CP-SAT
model.

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/fab/constraints/<subject>_cost.hpp` | new | | | weight |
| `cpp/include/fab/score_terms.hpp` | `Term`, `kTermCount` | | | |
| `cpp/include/fab/constraints.hpp` | include, `emplace` | | | |
| `cpp/src/score_calculator.cpp` | `rawCostsOf()` | | | `scoreOf()` |
| `cpp/src/evaluator.cpp`, `evaluator.hpp` | `recost()`, cache field | | a new unit, if any | |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | counts in the text |
| `tests/semiconductor_scheduling/cpp/fab_fixtures.hpp` | | `kTermNames` | | |
| `tests/semiconductor_scheduling/cpp/fab_test.cpp` | | case, count | | expected score |
| `tests/semiconductor_scheduling/cpp/fab_plugin_test.cpp` | | cases, counts, weights | | expected scores, row |
| `tests/semiconductor_scheduling/test_constraint_specs.py` | | lists | | row |
| `tests/semiconductor_scheduling/reference_semiconductor_scheduling.py` | | the constraint | | weight |
| `docs/use-cases/semiconductor-scheduling/README.md`, `python.md` | | table row | | defaults |
| external-solver, `benchmarks/semiconductor-scheduling` | | the same constraint | | results |

Moves and the construction are a different subject: see
[Extending](README.md#extending) in the guide.

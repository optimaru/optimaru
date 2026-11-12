# Knapsack: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its seven constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-three-functions) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | one C++ test case, the catalog in the tests, the Python reference scorer |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the two monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `unusedCapacityCost`, the capacity the selected items leave unused. It
is the other side of `knapsackCapacityCost`, which charges the weight over the capacity, and it
makes the search prefer a full knapsack. Every snippet below was compiled and run, and every figure
measured, on optimaru 0.4.3, except the `idle_at_weight_zero()` of step 4 and the figures with it
(and the seven alone they are compared with), on 0.5.1; a later change to the solver moves the
figures, not the steps.

## How a selection is scored

An item is selected or not. The engine keeps the score per **unit**, and here the
units are the two values: unit 1 holds the selected items, unit 0 the others. No
knapsack constraint is a sum of one term per unit: a capacity, a count per group and a
pair of items all read the selected set as a whole. So all seven constraints are
**global** components. Each keeps one raw total, is told about every item that
becomes selected or stops being selected, and is asked for its total at every score,
whatever its weight, unless it declares that it may sit out at weight 0 (step 4).

There are two scoring paths, and they give the same score for the same selection
(`ks_plugin_test` checks it on 500 random selections):

| | `knapsack.solve(problem)` | `knapsack.solve(problem, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/ks/constraints/` |
| Constraints | the seven, at their defaults | the ones you list, with your knobs |
| Steps per second | 2.2 million | 1.0 million |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are n500 of the benchmark (500 items), 5 million steps, one thread of an
Apple M4 Max; [step 6](#6-measure) has the script. Steps 1 to 4 work on the
assembly. Step 5 moves a constraint to the monolithic calculators.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import random

from optimaru import knapsack
from optimaru.knapsack.constraints import KnapsackCapacityCost, SelectedValueCost, get_constraints
from optimaru.knapsack.models import ItemModel, KnapsackProblemModel

rng = random.Random(4)  # n500 of the benchmark
items = [ItemModel(id=f"I{i}", weight=rng.randint(5, 60), value=rng.randint(10, 100)) for i in range(500)]
problem = KnapsackProblemModel(capacity=sum(item.weight for item in items) // 3, items=items)

changed = {"knapsack_capacity_cost", "selected_value_cost", "item_dependency_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    SelectedValueCost(weight=2),                                       # heavier (default 1)
    KnapsackCapacityCost(level="soft", weight=50),                     # hard by default
]                                                                      # item_dependency_cost: dropped
result = knapsack.solve(problem, time_limit_seconds=10, constraints=picks)
```

To try weights without solving again, score a finished selection with other picks:

```python
from optimaru.knapsack.constraints import score

raw = knapsack.constraint_costs(result, picks)  # raw total of every constraint
print(score(raw, picks))                        # the score those picks give this selection
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/knapsack/constraints.py).
Stop here if the constraint you need is one of the seven.

## 2. A new constraint from three functions

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

[`constraints/common.hpp`](../../../optimaru/knapsack/cpp/include/ks/constraints/common.hpp)
has one base, `detail::SelectionConstraint`, and all seven constraints derive from it. It
is the engine's `MemberChangeGlobalConstraint` narrowed to unit 1, and it asks for
three functions: `reset()` builds the total from the selection when the objective is
rebuilt, `onSelectionChanged()` updates it each time an item becomes selected or
stops being selected, and `cost()` returns it at every score.

Four edits:

**1. The component**, `optimaru/knapsack/cpp/include/ks/constraints/unused_capacity_cost.hpp`:

```cpp
#ifndef KS_CONSTRAINTS_UNUSED_CAPACITY_COST_HPP
#define KS_CONSTRAINTS_UNUSED_CAPACITY_COST_HPP

#include <cstdint>
#include <vector>

#include "ks/constraints/common.hpp"

namespace ks {

// Soft, off by default (weight 0): unusedCapacityCost — the capacity the
// selection leaves unused. Raw cost = max(0, capacity - sum of selected
// weights), one total; 0 = the knapsack is full (a selection over the
// capacity costs 0 here: the overflow is knapsackCapacityCost's). Keeps the
// selected load, O(1) per flip.
struct UnusedCapacityCost final : detail::SelectionConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit UnusedCapacityCost(double weight = kDefaultWeight)
        : detail::SelectionConstraint("unusedCapacityCost", weight, kDefaultLevel) {}

    // The aggregate from scratch: the load of the selected items.
    void reset(const KnapsackProblem& problem,
               const std::vector<std::vector<int>>& members_per_unit) override {
        detail::requirePrepared(problem);
        capacity_ = problem.capacity;
        load_ = 0;
        const std::vector<char> selected = detail::selectedFlags(problem, members_per_unit);
        for (std::size_t i = 0; i < selected.size(); ++i) {
            if (selected[i]) load_ += problem.items[i].weight;
        }
    }

    // One flip: the item's weight joins or leaves the load.
    void onSelectionChanged(const KnapsackProblem& problem, int item, bool selected) override {
        const std::int64_t weight = problem.items[item].weight;
        load_ += selected ? weight : -weight;
    }

    // The raw total, asked at every score.
    double cost() const override {
        const std::int64_t unused = capacity_ - load_;
        return unused > 0 ? static_cast<double>(unused) : 0.0;
    }

private:
    std::int64_t capacity_ = 0;
    std::int64_t load_ = 0;
};

}  // namespace ks

#endif  // KS_CONSTRAINTS_UNUSED_CAPACITY_COST_HPP
```

The name passed to the base, `"unusedCapacityCost"`, is the struct name with a
lower-case first letter. Python derives `unused_capacity_cost` from it. `reset()`
starts with `detail::requirePrepared(problem)`: the components read the indexes of
`ScoreCalculator::prepare()`, which every solve runs first. An undecided item (the
value -1, which ruin-and-recreate uses for a moment) is in neither unit, so
`detail::selectedFlags()` counts it as not selected.

The defaults live in these two constants and nowhere else; Python reads them through
`knapsack.constraint_info()`. `kDefaultLevel` is `Hard` or `Soft`: this domain has no
medium level, and `level="medium"` raises `ValueError`. A hard constraint returns whole
numbers, because a hard total is rounded once per constraint. A reward, a quantity
to maximize such as `selectedValueCost`, returns a raw cost of 0 or less and sets
`reward = true; level_flippable = false;` in its constructor: it can never be hard.

**2. The assembly**, `cpp/include/ks/constraints.hpp`: include the header, and add
one line to `defaultConstraints()`, before `SelectedValueCost`. The order there is
the order of the catalog, and the docs write `get_constraints()[:-1]` for "all but
the reward":

```cpp
    assembly.emplace<UnusedCapacityCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no default solve changes and the monolithic calculators stay as they are.

**3. The Python spec**, `optimaru/knapsack/constraints.py`:

```python
class UnusedCapacityCost(Constraint):
    """Capacity the selection leaves unused. Opt-in: soft, weight 0."""

    name = "unused_capacity_cost"
    label_en = "Unused capacity"
    label_ko = "남는 배낭 용량"
    description_en = (
        "The knapsack capacity the selected items leave unused. "
        "One unit of cost is one unit of weight below the capacity; 0 means the knapsack is full."
    )
    description_ko = (
        "선택한 물품의 무게 합이 배낭 용량에 못 미치는 양입니다. "
        "남는 무게 1단위가 비용 1이며, 0이면 배낭 용량을 남김없이 채운 것입니다."
    )
```

**4. Its default instance**, in the same file: `unused_capacity_cost =
UnusedCapacityCost()` with the other instances, that instance in `_DEFAULTS` before
`selected_value_cost`, and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=knapsack
uv sync                                    # rebuilds optimaru.knapsack._core
```

```python
from optimaru.knapsack.constraints import UnusedCapacityCost

others = [spec for spec in get_constraints() if spec.name != "unused_capacity_cost"]
picks = [*others, UnusedCapacityCost(weight=10)]
config = knapsack.SolverConfig()
config.step_count_limit = 2_000_000
config.random_seed = 1
result = knapsack.solve(problem, config=config, constraints=picks)
print(knapsack.constraint_costs(result, picks)["unused_capacity_cost"]["costs"], result.total_value)
```

On n500 that prints `[0.0] 17445`: the knapsack is full. A default solve of the same
length (`constraints=None`) leaves 2 of the capacity of 5,229 unused, at a value of
17,466. The constraint gave up 21 of value for the last two units.

What the base gave you: the engine reports every join and leave of both units, and
the base passes on those of unit 1. `onSelectionChanged()` runs once per flip, so
the constraint is incremental from the start. The engine never adds up a global constraint's
total itself: it asks `cost()`, so the total is exactly what the component keeps.

## 3. Check it

**A case worked out by hand**, in `tests/knapsack/cpp/ks_plugin_test.cpp`, with the
other components' cases in `verifyRawCostsByHand()`: a positive total and the score
it gives at a weight, then two zero cases (at exactly the capacity, and over it).

```cpp
        // unusedCapacityCost: what the selection leaves of capacity 10, and 0
        // from the capacity upwards. It is opt-in, so it scores at a weight.
        KnapsackProblem p = basicInstance();
        select(p, {1});  // B: 4 on 10
        check(rawCosts(p, "unusedCapacityCost") == Costs{6.0},
              "unusedCapacityCost: [6] (B, 4 on 10)");
        DecomposedCalculator calculator;
        defaultConstraints().weight("unusedCapacityCost", 3).into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{0, 7.0 - 18.0}),
              "unusedCapacityCost at weight 3: 3 x 6 soft off the value 7");
```

```sh
cmake -B build -DOPTIMARU_USE_CASES=knapsack && cmake --build build -j
ctest --test-dir build --output-on-failure
```

**The catalog.** An eighth constraint fails four checks of `ks_plugin_test` and twelve
Python tests, and they list more than a count. Update them:

- `ks_plugin_test.cpp`, `expectedTotals()` and `kCatalogNames` with the two loops
  over them. The test compares every raw total of the default assembly with the
  monolith's `RawCosts`, on 500 random selections and after each of 3,000 random
  moves. The monolith has no total for an opt-in constraint, so compute one in the test
  from the facts (the capacity minus `problem.totalWeight()`, floored at 0) at the
  constraint's place in `expectedTotals()`. The walk then checks the constraint's running total
  after every move, and that it is above 0 somewhere on the way.
- `ks_plugin_test.cpp`, `constraint_totals().size() == 7`, and `rows.size() == 7` in
  `verifyCatalog()`. Add the row before `selectedValueCost`. `rowIs()` expects a constraint
  that is on by default; make it compare `row.default_enabled` with `weight > 0.0`.
- `tests/knapsack/test_constraint_specs.py`, six tests: `NAMES`, the instances and
  the `constraint_info()` rows list the catalog,
  `test_constraint_costs_match_the_hand_calculation` lists every raw total of a
  fixture that leaves 7 of its capacity unused, and
  `test_weight_none_means_the_component_default` gave every constraint the weight 1 (read
  the default weights from `constraint_info()` there).
- `tests/knapsack/reference_knapsack.py`, the independent scorer in plain Python that
  six tests of `test_reference_parity.py` compare the engine with, per constraint.
  Add the constraint's name to `REFERENCE_NAMES` and `ENGINE_NAME`, its default to
  `REFERENCE_DEFAULTS`, and its total to `constraint_costs()`:

  ```python
          "unused_capacity": [max(0, problem["capacity"] - weight)],
  ```

Then add cases that pick the constraint, at a weight and on the hard level
(`test_levels_by_hand_on_both_paths` in `test_constraint_specs.py`).

**Incremental against full.** Solve with every score checked against a full recompute. It raises
`RuntimeError` ("Score corruption: ...", the engine's `ScoreMismatchError`) if a move leaves the
score stale:

```python
knapsack.solve(problem, time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

Give the constraint a weight for this: at weight 0 a stale total scores the same, and
with the `idle_at_weight_zero()` of step 4 the engine does not keep it during the moves at
all. The walk of `ks_plugin_test` compares raw totals, so it does not depend on the weight.

**The tables and the CP-SAT model.** The constraint table in [README.md](README.md)
must equal `knapsack.constraint_info()`; add the row. [python.md](python.md),
`__init__.py` and `_core.pyi` describe the score in words, and the notebook
`examples/knapsack.ipynb` prints the constraint list. The benchmark compares against
`external_solver.knapsack` in
[external-solver](https://github.com/optimaru/external-solver): one plugin per
constraint, a subclass of the Python spec with its defaults from `constraint_info()`.
Its `tests/knapsack/test_constraint_parity.py` runs over `list_constraints()` and pins
selections to compare each raw total, so it fails for a constraint without a plugin, opt-in
or not. Mirror the constraint there with an exact encoding.

## 4. Make it cheaper per move

On the assembly every flip calls `onSelectionChanged()` of every constraint in the picks,
and every score calls `cost()` of every constraint. A step is several of each: a move is
applied and scored, and undone when it is refused; a swap is two flips;
ruin-and-recreate moves a batch. On a random 200-item instance of the test fixtures,
1 million steps called `onSelectionChanged()` 6.9 million times and `cost()` 5.8
million times on each constraint. `reset()` ran twice, at the start and for the final
score. So a constraint is cheap when the first two are. What to keep depends on the constraint:

| The constraint is | Keep | Per flip | Example to copy |
| --- | --- | --- | --- |
| a sum of one number per selected item | one `int64` total | one addition | [`mandatory_items_cost.hpp`](../../../optimaru/knapsack/cpp/include/ks/constraints/mandatory_items_cost.hpp) |
| a threshold on such a sum | the sum; apply the threshold in `cost()` | one addition | step 2 |
| a sum over resources or groups of one threshold each | the amount per resource or group, and the summed total: take the old term out, change the amount, put the new term in | the item's resources or groups | [`resource_capacity_cost.hpp`](../../../optimaru/knapsack/cpp/include/ks/constraints/resource_capacity_cost.hpp) |
| a count over pairs of items | a copy of the selection, and the count | the item's pairs | [`item_dependency_cost.hpp`](../../../optimaru/knapsack/cpp/include/ks/constraints/item_dependency_cost.hpp) |

The example is the second row, and there is nothing left to cut: one addition per
flip, one comparison per score. What the other way costs: the same constraint with a
`cost()` that adds up the weights of all 500 items ran the assembly at 0.36 instead
of 1.0 million steps per second, and it did so at weight 0 too: the engine tells every
global constraint in the picks of every flip and asks it for its total at every score,
whatever its weight, because a global may keep something that more than its own total
reads. `get_constraints()` lists all eight, the example at weight 0.

**Let it sit out at weight 0.** A global may declare that the engine can leave it alone
while its weight is 0 when nothing but its own `cost()` reads what it keeps and `reset()`
rebuilds all of that from the selection. The example qualifies; add to it:

```cpp
    // Nothing but cost() reads the load, and reset() rebuilds it.
    bool idle_at_weight_zero() const override { return true; }
```

At weight 0 the engine then sends it no flip and does not ask for its total, and resets
it before `knapsack.constraint_costs()` reports the raw total. With it,
`get_constraints()` ran at 0.97 million steps per second, against 0.96 for the seven
alone in the same build. A constraint without the override at weight 0 costs what it
costs at a weight (step 6), so leave such a constraint out of the picks when you do not
use it.

Two more things keep a constraint on the cheap side.

**Read the per-item indexes.** `ScoreCalculator::prepare()` builds them once, when
a solve starts: `itemResources`, `itemGroups`, `mandatoryCount`, `forbiddenCount`,
`dependenciesAsItem` and `dependenciesAsRequired` (`KnapsackProblem` in
[`domain.hpp`](../../../optimaru/knapsack/cpp/include/ks/domain.hpp)). A flip then
visits what the item refers to and nothing else. A constraint that needs another index
adds it there, where the monolithic calculators can read it too (step 5), or builds
a table of its own in `reset()`. Under `full_assert` the engine rebuilds at every
score, 12,877 times in 2,000 steps on the instance above, so a heavy `reset()` only
slows a checked solve.

**Keep the aggregates whole numbers.** A refused move is undone by the inverse
flips, and the total has to come back exactly. An `int64` sum does. Convert in
`cost()` with `static_cast<double>`. A whole total is also what a hard constraint needs.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`knapsack.solve(problem)` must score it, and it scores with the monolithic
calculators: 2.2 times the steps per second of the assembly, and the only path that
runs parallel starts. `ks_plugin_test` compares the default assembly with them on
random selections, so it fails from the moment the weight is not 0 until both sides
agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 1.0;`.
It must be a whole number, because the monolithic calculators add in `int64`.

**2. The term**, in three files. Both calculators fill a `RawCosts`
(`cpp/include/ks/score_calculator.hpp`), and the tests compare the two field by
field. Add the field there, and to its `operator==`:

```cpp
    std::int64_t capacityUnused = 0;     // unusedCapacityCost
```

In `cpp/src/score_calculator.cpp`, the full recompute, include the header, read the
weight off the component (`kUnusedWeight`), and add it to the first `static_assert`
and its level to the second. Then the total in `rawCosts()`, stored in
`raw.capacityUnused`, and the term in `score()`:

```cpp
    // soft 7. unusedCapacityCost: the other side of the same load.
    const std::int64_t capacityUnused = load < problem.capacity ? problem.capacity - load : 0;
```

```cpp
    score.soft = static_cast<double>(kValueWeight * raw.value - kUnusedWeight * raw.capacityUnused);
```

In `cpp/src/incremental_calculator.cpp`: the same include, weight and `static_assert`,
the same line in `score()`, and the total in `totals()`:

```cpp
    raw.capacityUnused = load_ < problem_->capacity ? problem_->capacity - load_ : 0;
```

`flip()` stays as it is, because it already keeps `load_`. A constraint with an aggregate
of its own adds a member, fills it in `rebuild()` and updates it in `flip()`, the way
`groupCount_` and `groupViolation_` are kept. The default path ran at 2.2 million
steps per second with the eighth constraint in it, as before.

**3. The parity checks.** `expectedTotals()` in `ks_plugin_test.cpp` now reads
`raw.capacityUnused`, and the row in `verifyCatalog()` has the weight 1.
`catalogOrder()`, `kCatalogNames` and the loops of `verifyIncrementalWalk()` in
`tests/knapsack/cpp/ks_test.cpp` get the eighth entry: that walk compares the
incremental calculator with the full recompute, total by total. The Python reference
gets the weight in `REFERENCE_DEFAULTS`, and
`test_constraint_info_reports_the_cpp_defaults` the row.

**4. What else moves.** With that done the parity checks pass on both sides, and
every check that states a score at the defaults fails:

- `ks_test`: 19 checks, the hand-computed scores of `verifyHandCalculated()` and the
  empty problem of `verifySolver()`. "B+C fits, value 15" is now 14 soft, because B
  and C leave 1 of the capacity unused;
- `ks_plugin_test`: the 7 checks of `verifyPartialAssemblies()`;
- Python: five cases of `test_constraint_specs.py`
  (`test_constraint_costs_match_the_hand_calculation`, `test_level_can_be_flipped`,
  `test_reweighting_scales_only_that_term` and both cases of
  `test_the_same_literal_pick_on_soft_is_accepted`) and two tests of `test_solve.py`
  (`test_calculate_score_fills_the_model`, `test_an_empty_problem_solves`);
- the table and the counts in [README.md](README.md) and [python.md](python.md), and
  the constraint lists and counts in the comments of `common.hpp`, `constraints.hpp`,
  `score_calculator.hpp` and `domain.hpp` and in the docstrings of `constraints.py`,
  `__init__.py` and `_core.pyi`;
- the CP-SAT model: without the constraint at the same weight in `external_solver.knapsack`,
  [`benchmarks/knapsack`](../../../benchmarks/knapsack) no longer compares one
  objective. Its published results and `CHANGELOG.md` follow.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed:

```python
import time

steps = 5_000_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    core = problem.to_core()
    config = knapsack.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    knapsack.solve(core, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e6:5.2f} M steps/s  {core.score}")
```

Both lines end on the same score: same seed, same steps, same scoring. If they
differ after a change, the two paths no longer score alike.

| Measured on n500, median of three runs | Steps per second |
| --- | --- |
| default path, seven constraints | 2.20 million |
| assembly, seven constraints | 1.02 million |
| assembly, seven + the constraint of step 2 at weight 0 | 0.98 million |
| assembly, seven + the constraint of step 2 at weight 10 | 1.01 million |
| assembly, seven + the constraint with `idle_at_weight_zero()` (step 4) at weight 0 (the seven, 0.96) | 0.97 million |
| assembly, the same constraint with a `cost()` over all items (step 4) | 0.36 million |
| default path, eight constraints (step 5) | 2.22 million |
| assembly, eight constraints (step 5) | 0.99 million |

The row with `idle_at_weight_zero()` was taken on 0.5.1, next to the seven alone in the
same build (in brackets): at weight 0 the constraint then costs a move nothing, so it is the
speed of the assembly without it.

**What it buys.** A constraint changes what the search can reach, so compare the results
at the same budget over several seeds, with and without it:

```python
for seed in range(1, 6):
    config = knapsack.SolverConfig()
    config.step_count_limit = 2_000_000
    config.random_seed = seed
    for picked in (others, picks):
        result = knapsack.solve(problem, config=config, constraints=picked)
        print(seed, result.total_value, problem.capacity - result.total_weight)
```

Each line is the value and the unused capacity, without the constraint and then with it
at weight 10. On n500 the five solves without the constraint leave 2, 2, 5, 5 and 5 of the
capacity unused; with it they leave 0, 1, 0, 0 and 0, at 6 to 34 less value. A step
limit makes the lines repeatable, and on the assembly both solves take the same time.

Against CP-SAT at a fixed time: `uv run knapsack-compare --seconds 10 --workers 8` in
`benchmarks/knapsack/cpsat` ([README](../../../benchmarks/knapsack/README.md)). It
solves on the default path only, so it measures a constraint after step 5 and with the
constraint in the CP-SAT model.

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/ks/constraints/<subject>_cost.hpp` | new | | the totals it keeps, `idle_at_weight_zero()` | weight |
| `cpp/include/ks/constraints.hpp` | include, `emplace` | | | |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | counts in the text |
| `tests/knapsack/cpp/ks_plugin_test.cpp` | | case, totals, counts | | totals, row |
| `tests/knapsack/test_constraint_specs.py` | | lists, cases | | row, expected scores |
| `tests/knapsack/reference_knapsack.py` | | the constraint | | weight |
| `docs/use-cases/knapsack/README.md`, `python.md` | | table row | | defaults |
| `cpp/include/ks/score_calculator.hpp` | | | | `RawCosts` field |
| `cpp/src/score_calculator.cpp`, `incremental_calculator.cpp` | | | | the term |
| `tests/knapsack/cpp/ks_test.cpp` | | | | totals, expected scores |
| external-solver, `benchmarks/knapsack` | | the same constraint | | results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

# Bin packing: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its four constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | hand cases in the C++ test, the reference scorer, the counts and lists in the tests |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the two monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `loadBalanceCost`, the load a bin in use is short of the even share.
The even share is the total item weight divided by the fewest bins that could hold it. The
constraint evens out the loads of the bins in use. Every snippet below was compiled and run, and
every figure measured, on optimaru 0.4.3; a later change to the solver moves the figures, not the
steps.

## How a packing is scored

An item is assigned a bin. There are as many bins as items, all alike. The score is
kept per **unit**: one bin, with the items assigned to it as its **members**. A
change moves one item and a swap two, so only the bins they leave and join are
scored again. The two macro moves of the domain (`EmptyBinMove`, `RepackMove`) move
several items in one step.

There are two scoring paths, and they give the same score for the same packing
(`bp_plugin_test` checks it on 500 random assignments):

| | `bp.solve(problem)` | `bp.solve(problem, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/bp/constraints/` |
| Constraints | the four, at their defaults | the ones you list, with your knobs |
| Steps per second | 0.96 million | 0.89 million |
| Steps per second, macro moves off | 17.9 million | 7.1 million |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are a generated instance of 120 items ("sq1" of the benchmark, 43 bins at
best), one thread of an Apple M4 Max; [step 6](#6-measure) has the script. In the
default search the two macro moves take most of a step, so the two paths are close.
With them off (`empty_bin_moves`, `repack_moves`) a step is a change or a swap and
scoring is most of it: the monolithic calculators are then 2.5 times as fast. That
search is the worse one, though: it ends on 45 bins after 10 million steps where the
default search reaches 43 in 2 million. Steps 1 to 4 work on the assembly. Step 5
moves a constraint to the monolithic calculators.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import json
from pathlib import Path

import optimaru.bin_packing as bp
from optimaru.bin_packing.constraints import ColorConflictCost, UsedBinsCost, get_constraints
from optimaru.bin_packing.models import BinPackingProblemModel

demo = json.loads(Path("tests/bin_packing/fixtures/web_demo.json").read_text())
problem = BinPackingProblemModel.model_validate(demo).to_core()  # 20 items, capacity 100
changed = {"color_conflict_cost", "used_bins_cost", "item_conflict_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    UsedBinsCost(weight=10),                                           # heavier (default 1)
    ColorConflictCost(level="soft"),                                   # hard by default
]                                                                      # item_conflict_cost: dropped
bp.solve(problem, time_limit_seconds=2, constraints=picks)             # solves in place
```

To try weights without solving again, score a finished packing with other picks:

```python
from optimaru.bin_packing.constraints import score

raw = bp.constraint_costs(problem, picks)  # raw cost of every constraint, per bin
print(score(raw, picks))                   # the score those picks give this packing
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/bin_packing/constraints.py).
Stop here if the constraint you need is one of the four.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

There is no base that asks for one term per item: the bins are alike, so nothing
depends on one item and its bin alone. The bases are the engine's two, under the
aliases of
[`constraints/common.hpp`](../../../optimaru/bin_packing/cpp/include/bp/constraints/common.hpp).
Pick the base by what the constraint is:

| The constraint is | Base | You write |
| --- | --- | --- |
| a cost per bin, read off the items in it | `BpUnitConstraint` | `cost(problem, bin, members)` |
| one number for the whole packing (a count of bins) | `BpGlobalConstraint` | `reset()`, `on_unit_changed()`, `cost()` |

The exponent, offset and clip of a pick shape each bin's cost for the first kind and
the one total for the second. That is why `usedBinsCost` is a global: its exponent
shapes the bin count.

The example is a cost per bin, so it is one function. Four edits:

**1. The component**, `optimaru/bin_packing/cpp/include/bp/constraints/load_balance_cost.hpp`:

```cpp
#ifndef BP_CONSTRAINTS_LOAD_BALANCE_COST_HPP
#define BP_CONSTRAINTS_LOAD_BALANCE_COST_HPP

#include "bp/constraints/common.hpp"

namespace bp {

// Soft, off by default (weight 0): loadBalanceCost — the load a bin in use is
// short of the even share. The even share is the total item weight divided by
// the fewest bins that could hold it, ceil(total / binCapacity), rounded
// down. Raw cost per bin = max(0, share - load) for a bin holding at least one
// item, so 0 = every bin in use carries its share (an empty bin costs 0).
//
// No delta contract: the clamp makes the cost a function of the whole bin,
// not a sum of per-item terms (see binCapacityCost).
struct LoadBalanceCost final : BpUnitConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit LoadBalanceCost(double weight = kDefaultWeight)
        : BpUnitConstraint("loadBalanceCost", weight, kDefaultLevel) {}
    void on_reset(const BinPackingProblem& problem) override { detail::requirePrepared(problem); }
    double cost(const BinPackingProblem& problem, int /*unit*/,
                const std::vector<int>& members) const override {
        if (members.empty()) return 0.0;
        std::int64_t total = 0;
        for (const Item& item : problem.items) total += item.weight;
        const std::int64_t fewestBins = (total + problem.binCapacity - 1) / problem.binCapacity;
        const std::int64_t share = total / fewestBins;
        std::int64_t load = 0;
        for (int item : members) load += problem.items[item].weight;
        return load < share ? static_cast<double>(share - load) : 0.0;
    }
};

}  // namespace bp

#endif  // BP_CONSTRAINTS_LOAD_BALANCE_COST_HPP
```

The name passed to the base, `"loadBalanceCost"`, is the struct name with a
lower-case first letter. Python derives `load_balance_cost` from it. `on_reset()` is
the line every component here has: it refuses a problem that
`ScoreCalculator::prepare()` has not checked (capacity and weights above 0).

The defaults live in the two constants and nowhere else; Python reads them through
`bp.constraint_info()`. `kDefaultLevel` is `Hard` or `Soft`: bin packing has no
medium level (its `Score` in `domain.hpp` is the engine's two-level score), and a
pick with `level="medium"` raises `ValueError`. A hard constraint returns whole numbers,
because a hard contribution is rounded per bin; every constraint here counts weight, items
or bins, so return `static_cast<double>` of an integer.

**2. The assembly**, `cpp/include/bp/constraints.hpp`: include the header, and add
one line at the end of `defaultConstraints()`:

```cpp
    assembly.emplace<LoadBalanceCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no default solve changes and the monolithic calculators stay as they are.

**3. The Python spec**, `optimaru/bin_packing/constraints.py`:

```python
class LoadBalanceCost(Constraint):
    """Load a bin in use is short of the even share, per bin. Opt-in: soft, weight 0."""

    name = "load_balance_cost"
    label_en = "Load balance"
    label_ko = "상자 무게 균형"
    description_en = (
        "For each bin in use: how far its load is below the even share, the total item weight divided by "
        "the fewest bins that could hold it. 0 means every bin in use carries at least its share."
    )
    description_ko = (
        "사용한 상자마다 담긴 무게가 균등 몫에 못 미치는 양을 더합니다. 균등 몫은 전체 물건 무게를 "
        "그 무게를 담을 수 있는 최소 상자 수로 나눈 값입니다. 0이면 사용한 상자가 모두 균등 몫 이상을 담고 있습니다."
    )
```

**4. Its default instance**, in the same file: `load_balance_cost =
LoadBalanceCost()` with the other instances, that instance in `_DEFAULTS` before
`used_bins_cost` (the catalog lists the per-bin constraints first, the global last), and
both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=bin_packing
uv sync                                    # rebuilds optimaru.bin_packing._core
```

```python
from optimaru.bin_packing.constraints import LoadBalanceCost

others = [spec for spec in get_constraints() if spec.name != "load_balance_cost"]
picks = [*others, LoadBalanceCost(weight=1)]
config = bp.SolverConfig()
config.step_count_limit = 200_000
config.random_seed = 1
problem = BinPackingProblemModel.model_validate(demo).to_core()
bp.solve(problem, config=config, constraints=picks)
print(sum(bp.constraint_costs(problem, picks)["load_balance_cost"]["costs"]), problem.bin_loads())
```

On the demo instance that sum is 4, with bin loads from 91 to 97. A default solve of
the same length (`constraints=None`) leaves it at 22, with loads from 84 to 100.
Both use 8 bins.

What the engine does with the component: it keeps the constraint's raw cost of every bin,
and when an item moves it calls `cost()` again for the bin the item leaves and the
bin it joins. The constraint is already incremental per bin.

## 3. Check it

**Cases worked out by hand**, in `tests/bin_packing/cpp/bp_plugin_test.cpp`, with
the other raw costs in `verifyRawCostsByHand()`: two positive cases and a zero case.

```cpp
    // loadBalanceCost: 20 of weight on capacity 10 fits 2 bins at best, so the
    // even share is 10.
    check(rawCosts(fourItems({0, 0, 1, 1}), "loadBalanceCost") == std::vector<double>{0, 5, 0, 0},
          "loadBalanceCost: {C3,D2} is 5 short of the share -> [0, 5, 0, 0]");
    check(rawCosts(fourItems({0, 1, 2, 2}), "loadBalanceCost") == std::vector<double>{2, 3, 5, 0},
          "loadBalanceCost: {A8} {B7} {C3,D2} -> [2, 3, 5, 0], the empty bin costs 0");
    check(rawCosts(fourItems({0, 1, 1, 0}), "loadBalanceCost") == std::vector<double>{0, 0, 0, 0},
          "loadBalanceCost: both bins carry 10 -> [0, 0, 0, 0]");
```

A fourth case after them scores the first packing twice: the default assembly gives
-5hard/-2soft, the constraint at weight 2 gives -5hard/-12soft.

```sh
cmake -B build -DOPTIMARU_USE_CASES=bin_packing && cmake --build build -j
ctest --test-dir build --output-on-failure        # bp_test and bp_plugin_test
uv run pytest -q tests/bin_packing tests/test_constraints_contract.py
```

**The counts.** A fifth constraint fails four checks of `bp_plugin_test` and 26 Python
tests that count or list the catalog. Update them:

- `bp_plugin_test.cpp`, the catalog: `constraint_totals().size() == 4`, and
  `rows.size() == 4` in `verifyCatalog()`. The new row is `rows[3]`, before
  `usedBinsCost`. `rowIs()` expects a constraint that is on by default, so check the new
  row's fields directly (`!rows[3].default_enabled`);
- `bp_plugin_test.cpp`, the comparison with the monolithic calculators:
  `verifyAssemblyParity()` and `walkIncremental()` compare `sums()`, each
  component's summed bin costs, with the four totals of `RawCosts`. The scores still
  agree; the lists no longer line up. Leave the opt-in constraint out of `sums()`, and
  add it to `kConstraintNames` and `nonZero` so the walk still checks that every
  constraint is non-zero at some step;
- `tests/bin_packing/test_constraint_specs.py`: `NAMES`, the instance lists in
  `test_get_constraints_defaults_to_the_full_set` and
  `test_module_level_instances_keep_every_default`, the rows of
  `test_constraint_info_reports_the_cpp_defaults`, and the raw costs of
  `test_the_hand_instance`.

**The reference scorer.** `tests/bin_packing/reference_bin_packing.py` is a second
implementation in plain Python, and `test_reference_parity.py` compares every
constraint's total with it on samples and on 600 random assignments. Add the constraint
there, written independently: its name in `REFERENCE_NAMES` and `ENGINE_NAME`, its
entry in `constraint_costs()` with a default weight of 0, and its spec in `SPEC_OF`
of the parity test.

**Incremental against full.** Solve with every score checked against a full recompute. It raises
`RuntimeError` ("Score corruption: ...", the engine's `ScoreMismatchError`) if a move leaves the
score stale:

```python
bp.solve(problem, time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

`bp_plugin_test` does the same for the default assembly, which now holds the constraint:
5,000 random moves and two walks of the macro moves, every bin's cost against a
fresh assembly after each.

**The tables.** The constraint tables in [README.md](README.md) and
[python.md](python.md) must equal `bp.constraint_info()`; add the row. The notebook
`examples/bin-packing.ipynb` prints the constraint list.

## 4. Make it cheaper per move

On the assembly a move costs, for every constraint, one call for the bin an item leaves
and one for the bin it joins. What that call is depends on the constraint:

| The constraint is | Use | Per move | Example to copy |
| --- | --- | --- | --- |
| read off the whole bin (a clamp, a threshold) | `BpUnitConstraint` with `cost()` | one pass over the items of the bin | [`bin_capacity_cost.hpp`](../../../optimaru/bin_packing/cpp/include/bp/constraints/bin_capacity_cost.hpp), step 2 |
| a sum of terms per item or per pair of items in one bin | the same, with `supports_delta()` and `delta_cost()` | one pass over the bin's other items, for the moved item only | [`color_conflict_cost.hpp`](../../../optimaru/bin_packing/cpp/include/bp/constraints/color_conflict_cost.hpp), [`item_conflict_cost.hpp`](../../../optimaru/bin_packing/cpp/include/bp/constraints/item_conflict_cost.hpp) |
| one number across bins | `BpGlobalConstraint` with `supports_member_change()` and `on_member_changed()`: keep your own totals | what your update costs | [`used_bins_cost.hpp`](../../../optimaru/bin_packing/cpp/include/bp/constraints/used_bins_cost.hpp) |

A unit constraint with `cost()` alone is correct: the engine then recomputes the
whole bin on every change. `delta_cost()` returns what one item adds given the
others, and must equal `cost(with the item) - cost(without it)` exactly;
`full_assert` (step 3) checks that it does. The example gains nothing from one: what
an item adds depends on the whole load, and a bin holds few items (2.8 on average on
the measured instance). What it can save is the part that does not depend on the bin.

**Compute once per solve.** The facts never change during a solve, so what reads
only facts can be computed once. `on_reset()` runs when the solve starts:

```cpp
    // One number per solve: cost() below is then one pass over the bin's items.
    void on_reset(const BinPackingProblem& problem) override {
        detail::requirePrepared(problem);
        share_ = evenShare(problem);
    }
    double cost(const BinPackingProblem& problem, int /*unit*/,
                const std::vector<int>& members) const override {
        if (members.empty()) return 0.0;
        std::int64_t load = 0;
        for (int item : members) load += problem.items[item].weight;
        return load < share_ ? static_cast<double>(share_ - load) : 0.0;
    }

    // The loop of step 2, moved here unchanged (0 for a problem without items).
    static std::int64_t evenShare(const BinPackingProblem& problem) {
        std::int64_t total = 0;
        for (const Item& item : problem.items) total += item.weight;
        if (total == 0) return 0;
        const std::int64_t fewestBins = (total + problem.binCapacity - 1) / problem.binCapacity;
        return total / fewestBins;
    }

private:
    std::int64_t share_ = 0;  // the even share of this solve's problem
```

With the constraint at weight 1 and the macro moves off, the assembly went from 5.6 to 6.2
million steps per second, and the solve ended on the same score, as it must. In the
default search the same change is within the spread of the runs (0.90 and 0.92
million): the macro moves take most of a step there. A solve calls `on_reset()`
twice, at its start and on its result. Under `full_assert` the engine rebuilds at
every score, so `on_reset()` runs at every step there; 20,000 checked steps still
take 0.03 seconds on the demo instance.

**A constraint in the picks is evaluated, whatever its weight.** `get_constraints()` now
returns five picks, the new constraint at weight 0 among them, and the assembly calls its
`cost()` on every move: 6.2 million steps per second with it, 7.1 million with the
four others alone (macro moves off). Leave a constraint you do not use out of the picks.

**Scratch buffers.** `ColorConflictCost` and `ItemConflictCost` size a `mutable`
buffer in `on_reset()` and leave it zeroed after every call, so no call allocates.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`bp.solve(problem)` must score it, and `bp.solve(problem)` scores with the
monolithic calculators: the only path that runs parallel starts, and 2.5 times the
steps per second of the assembly where scoring is what a step costs.

`bp_plugin_test` compares the default assembly with the monolithic calculators on
random assignments, so it fails from the moment the weight is not 0 until both sides
agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 1.0;`.
It must be a whole number, because the monolithic calculators add in `int64`.

**2. The term**, in both calculators. They read the weight off the component and the
share from its `evenShare()`, so neither is written twice.

The full recompute, `cpp/src/score_calculator.cpp`: include the header, add
`kLoadBalanceWeight` next to the other weights and to their `static_assert`. `RawCosts`
(`score_calculator.hpp`) gets a field `loadBalance`, before `usedBins`. The loop over
the bins in `ScoreCalculator::rawCosts()` adds `share - load[b]` for a bin in use
below the share, and `ScoreCalculator::score()` weighs the field into the soft level.

The incremental one, `cpp/src/incremental_calculator.cpp` and its header. It keeps
the load of every bin, so the constraint is one more term that is taken out before the
load changes and put back after it, in `insert()` and in `retract()`, the way
`binCapacityCost` is kept:

```cpp
    // Every weight is > 0, so a bin is in use exactly when its load is > 0.
    std::int64_t balancePenalty(int bin) const {
        return load_[bin] > 0 && load_[bin] < share_ ? share_ - load_[bin] : 0;
    }
```

```cpp
    overCapacity_ -= capacityPenalty(bin);
    belowShare_ -= balancePenalty(bin);
    load_[bin] += item.weight;
    overCapacity_ += capacityPenalty(bin);
    belowShare_ += balancePenalty(bin);
```

`rebuild()` sets `share_ = LoadBalanceCost::evenShare(solution)` and `belowShare_ =
0`, `totals()` reports `belowShare_` as `loadBalance`, and `score()` weighs it. The
update is a few integer operations per move: with the macro moves off the default
path ran at 16.7 million steps per second with the fifth constraint in it, against 17.9
million with four.

A constraint that is not read off a bin's load or item count is more work: the
incremental calculator keeps no list of the items of a bin, so such a constraint brings
its own state per bin, as the colour constraint does (`colorCount()`).

**3. The tests of both sides.** In `bp_plugin_test.cpp`, `asList()` gets the new
field and `sums()` lists every component again, so the constraint's raw total is compared
too. In the reference scorer the constraint's default weight becomes 1. With that, the
comparisons of the plugin test and all of `test_reference_parity.py` pass.

**4. What else moves.** A constraint that is on by default changes every default score:

- hand-computed scores: 12 checks of `tests/bin_packing/cpp/bp_test.cpp` (the hand
  cases, the two 20-item samples, the macro moves by hand), 6 of
  `bp_plugin_test.cpp` (the partial assemblies and one override), and 9 Python tests
  that state the defaults or score or solve at them (`test_bin_packing.py`,
  `test_constraint_specs.py`, `test_solve.py`). The two checks of step 3 that say
  the constraint is off fail as well;
- `bp_test.cpp` tests the monolithic calculators alone: give it a positive and a
  zero case in `verifyConstraintsByHand()`, and the new field in its `asList()`;
- the tables, the score formulas and the counts ("four", "the three per-bin
  constraints") in [README.md](README.md), [python.md](python.md), `constraints.py`,
  `__init__.py`, `_core.pyi`, `domain.hpp`, `score_calculator.hpp`,
  `constraints.hpp` and `constraints/common.hpp`;
- the CP-SAT model the benchmark compares against, `external_solver.bin_packing` in
  [external-solver](https://github.com/optimaru/external-solver): a plugin that
  subclasses the spec and takes its defaults from `constraint_info()`, and its place
  in `REGISTRY`. That repository's `tests/bin_packing/test_constraint_parity.py`
  pins assignments in CP-SAT and compares every name of `list_constraints()` with
  the engine's raw cost, so it asks for the plugin from step 2 on. Without the same
  constraint there, [`benchmarks/bin-packing`](../../../benchmarks/bin-packing) no longer
  compares one objective: CP-SAT then minimizes the bins alone;
- the published results in `benchmarks/bin-packing/README.md`, the recorded baseline
  of `bench/quality.py`, and `CHANGELOG.md`.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed, once
with the default search and once with the macro moves off:

```python
import sys
import time

sys.path.insert(0, "benchmarks/bin-packing")  # bench.quality: the generated instances

import optimaru.bin_packing as bp
from bench.quality import review_instance
from optimaru.bin_packing.constraints import get_constraints
from optimaru.bin_packing.models import BinPackingProblemModel

data = review_instance(20261031)  # "sq1": 120 items, 4 colours, 10 conflict pairs
for macro_moves, steps in ((True, 2_000_000), (False, 10_000_000)):
    for label, picked in (("default path", None), ("assembly", get_constraints())):
        problem = BinPackingProblemModel.model_validate(data).to_core()
        config = bp.SolverConfig()
        config.step_count_limit = steps
        config.spent_limit_seconds = 600
        config.random_seed = 1
        config.empty_bin_moves = config.repack_moves = macro_moves
        start = time.perf_counter()
        bp.solve(problem, config=config, constraints=picked)
        seconds = time.perf_counter() - start
        print(f"{label:14s} macro moves {macro_moves!s:5s} {steps / seconds / 1e6:5.2f} M steps/s  {problem.score}")
```

The two lines of a search end on the same score: same seed, same steps, same
scoring. If they differ after a change, the two paths no longer score alike.

| Measured on sq1, million steps per second | Default search, 2 million steps | Macro moves off, 10 million steps |
| --- | --- | --- |
| default path, four constraints | 0.96 | 17.9 |
| assembly, four constraints | 0.89 | 7.1 |
| assembly, four + the constraint of step 2 | 0.90 | 5.6 |
| assembly, four + the constraint with the share of step 4 | 0.92 | 6.2 |
| default path, five constraints (step 5) | 1.01 | 16.7 |

Each figure is the median of three runs. Read what a constraint costs per move in the
right-hand column: in the left-hand one the macro moves cost more than scoring, and a
constraint with a weight changes which moves are taken.

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
bins at the same budget over several seeds, before and after:

```sh
cd benchmarks/bin-packing
export OPTIMARU_USE_CASES=bin_packing      # keep it set: uv rebuilds when it changes
uv sync --project cpsat
uv run --project cpsat python -m bench.quality --seconds 2 --seeds 1,2,3    # bins per seed
uv run --project cpsat python -m bin_packing_cpsat.benchmark --seconds 1 --workers 2 --cpsat-grace 20
```

`bench.quality` solves generated instances on the default path only, so it measures
a constraint after step 5; `--check` fails when a run uses more bins than the recorded
baseline. With `loadBalanceCost` on by default its eight instances still ended on
their lower bounds. The last command is the comparison with CP-SAT on generated
instances, and `python -m bench.run_bench` the one on public instances
([README](../../../benchmarks/bin-packing/README.md)).

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/bp/constraints/<subject>_cost.hpp` | new | | `on_reset` | weight |
| `cpp/include/bp/constraints.hpp` | include, `emplace` | | | |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | docstring, counts in the text |
| `tests/bin_packing/cpp/bp_plugin_test.cpp` | | cases, counts, `sums()` | | `asList()`, `sums()`, expected scores |
| `tests/bin_packing/test_constraint_specs.py` | | lists | | expected scores |
| `tests/bin_packing/reference_bin_packing.py`, `test_reference_parity.py` | | the constraint, weight 0 | | weight |
| `docs/use-cases/bin-packing/README.md`, `python.md` | | table row | | defaults |
| `cpp/src/score_calculator.cpp`, `cpp/include/bp/score_calculator.hpp` | | | | the term, `RawCosts` |
| `cpp/src/incremental_calculator.cpp`, `cpp/include/bp/incremental_calculator.hpp` | | | | the term |
| `tests/bin_packing/cpp/bp_test.cpp`, `test_bin_packing.py`, `test_solve.py` | | | | expected scores, cases |
| external-solver, `benchmarks/bin-packing` | | | | the same constraint, results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

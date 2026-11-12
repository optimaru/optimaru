# Order picking: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its four constraints are in the [guide](README.md), [python.md](python.md)
and [cpp.md](cpp.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | one C++ test case, a catalog row, a count, three lists in the tests |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the two monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `orderRevisitCost`, the picks that bring a trolley back to an order it
already left. Orders never share a bucket, so a picker who jumps from order to order and back has to
find the right bucket again. The four constraints price the walk, the buckets and the splits; none
prices the order the picks come in. Every snippet below was compiled and run, and every figure
measured, on optimaru 0.5.1; a later change to the solver moves the figures, not the steps.

## How a plan is scored

A pick task is given a place on a trolley's pick list. The score is kept per
**unit**: one trolley, with the pick tasks of its list, in picking order, as its
**members**. There is no pool: after construction every pick is on some trolley. A
move changes one or two lists, so only those are scored again. All four constraints are
sums of one term per trolley (the order split counts the orders on each trolley), so
the assembly has no global constraint.

There are two scoring paths, and they give the same score for the same plan
(`op_plugin_test` checks it on 300 random list layouts and over 2,000 random list
moves):

| | `op.solve(solution)` | `op.solve(solution, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/op/constraints/` |
| Constraints | the four, at their defaults | the ones you list, with your knobs |
| Steps per second | 0.86 million | 2.20 million |
| Moves | the built-in list moves and `OrderConsolidateMove` | the built-in list moves |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are the demo warehouse made larger, `op.build_demo_data(trolleys_count=10,
orders_count=24)` (24 orders, 304 picks, 10 trolleys of 4 buckets), 3 million steps,
one thread of an Apple M4 Max; [step 6](#6-measure) has the script. Steps 1 to 4 work
on the assembly. Step 5 moves a constraint to the monolithic calculators. The two paths do
not run the same search, which is why the assembly takes more steps per second:

- The assembly **previews** a move: since no constraint is a global, the engine asks every
  constraint with a weight what the changed lists would cost, and applies only the move it
  accepts. The monolithic incremental calculator has no previews, so the default path
  applies, scores and undoes every candidate.
- `OrderConsolidateMove`, which gathers a split order onto one trolley, runs on the
  default path only: it reads the monolithic calculator's per-(order, trolley) volume
  table.
- So with one seed and a step limit each path repeats exactly, but the two end on
  different plans (`-25456soft` and `-29552soft` on the instance above). That the two
  score alike is what `op_plugin_test` checks.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import optimaru.order_picking as op
from optimaru.order_picking.constraints import OrderSplitByTrolleyCost, TrolleyCapacityCost, get_constraints

solution = op.build_demo_data(trolleys_count=10, orders_count=24)
changed = {"order_split_by_trolley_cost", "trolley_capacity_cost", "distance_from_last_pick_task_to_path_origin_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    OrderSplitByTrolleyCost(weight=100),                               # lighter (default 1000)
    TrolleyCapacityCost(level="soft", weight=5000),                    # hard by default
]                                                                      # the walk back: dropped
op.solve(solution, time_limit_seconds=10, constraints=picks)          # solves in place
```

To try weights without solving again, score a finished plan with other picks:

```python
from optimaru.order_picking.constraints import score

raw = op.constraint_costs(solution, picks)  # raw cost of every constraint, per trolley
print(score(raw, picks))                    # the (hard, soft) those picks give this plan
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/order_picking/constraints.py).
The domain has two levels: `level="medium"` raises `ValueError`. Stop here if the constraint
you need is one of the four.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

Every constraint here derives from `OpUnitConstraint`
([`constraints/common.hpp`](../../../optimaru/order_picking/cpp/include/op/constraints/common.hpp)),
and you write one function, `cost(solution, unit, members)`: the raw cost of trolley
`unit` picking `members` in that order. It reads the solution's facts and its
arguments, and never the derived fields of a `PickTask` (`trolleyIndex`,
`previousPickTask`, `nextPickTask`): those describe the lists as they were last
scored, and a preview asks about lists that do not exist yet. A pick knows its
order (`PickTask::orderIndex`), its volume and its location; `calculateDistance()` in
`domain.hpp` gives the warehouse distance between two locations. A constraint that does not
decompose per trolley is a `OpGlobalConstraint`; [step 4](#4-make-it-cheaper-per-move)
says what that costs the assembly.

The example reads one list, so it is one function. Four edits:

**1. The component**, `optimaru/order_picking/cpp/include/op/constraints/order_revisit_cost.hpp`:

```cpp
#ifndef OP_CONSTRAINTS_ORDER_REVISIT_COST_HPP
#define OP_CONSTRAINTS_ORDER_REVISIT_COST_HPP

#include "op/constraints/common.hpp"

namespace op {

// Soft, weight 0 (opt-in, default_enabled = false): orderRevisitCost — a
// trolley that leaves an order for another and comes back to it later. Each
// order has its own bucket, so a picker who jumps between orders has to find
// the right bucket again. Raw cost per trolley = the picks whose order is
// not the previous pick's but was picked earlier on this trolley; 0 = every
// order's picks follow each other. Give it a weight to pick order by order.
struct OrderRevisitCost final : OpUnitConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit OrderRevisitCost(double weight = kDefaultWeight)
        : OpUnitConstraint("orderRevisitCost", weight, kDefaultLevel) {}
    double cost(const OrderPickingSolution& solution, int /*unit*/,
                const std::vector<int>& members) const override {
        std::vector<char> left(solution.orders.size(), 0);  // order picked, then left
        std::int64_t revisits = 0;
        int previous = -1;
        for (int pickIndex : members) {
            const int order = solution.pickTasks[pickIndex].orderIndex;
            if (order == previous) continue;
            if (left[order]) ++revisits;
            if (previous >= 0) left[previous] = 1;
            previous = order;
        }
        return static_cast<double>(revisits);
    }
};

}  // namespace op

#endif  // OP_CONSTRAINTS_ORDER_REVISIT_COST_HPP
```

The name passed to the base, `"orderRevisitCost"`, is the struct name with a
lower-case first letter. Python derives `order_revisit_cost` from it.

The defaults live in these two constants and nowhere else; Python reads them through
`op.constraint_info()`. `kDefaultLevel` is `Hard` or `Soft` (the score is the engine's
`Score` as it is: hard buckets short, soft meters and order splits), and the weight is
in units of that level. A hard constraint returns whole numbers, because a hard contribution
is rounded per trolley.

**2. The assembly**, `cpp/include/op/constraints.hpp`: include the header, and add
one line at the end of `defaultConstraints()`:

```cpp
    assembly.emplace<OrderRevisitCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no solve changes and the monolithic calculators stay as they are.

**3. The Python spec**, `optimaru/order_picking/constraints.py`:

```python
class OrderRevisitCost(Constraint):
    """Picks that return to an order the trolley already left. Default: soft, weight 0 (opt-in)."""

    name = "order_revisit_cost"
    label_en = "Orders picked in pieces"
    label_ko = "끊어서 피킹한 주문"
    description_en = (
        "For each trolley, the picks whose order is not the previous pick's but was picked "
        "earlier on that trolley: each order has its own bucket, and every return means finding "
        "it again. One unit of cost is one return; 0 means every order's picks follow each other. "
        "Opt-in: the default weight is 0."
    )
    description_ko = (
        "트롤리마다, 바로 앞 피킹과 주문이 다르지만 그 트롤리에서 앞서 피킹한 적이 있는 주문의 피킹 "
        "수를 셉니다. 주문마다 버킷이 따로 있어 다시 돌아올 때마다 그 버킷을 찾아야 합니다. 한 번 "
        "돌아오는 것이 비용 1이며, 0이면 모든 주문의 피킹이 이어서 이루어집니다. 기본 가중치가 0이므로 "
        "쓰려면 가중치를 직접 지정해야 합니다."
    )
```

**4. Its default instance**, in the same file: `order_revisit_cost = OrderRevisitCost()`
with the other instances, that instance at the end of `_DEFAULTS` (the
`list_constraints()` order), and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=order_picking
uv sync --reinstall-package optimaru       # rebuilds optimaru.order_picking._core
```

```python
from optimaru.order_picking.constraints import OrderRevisitCost

others = [spec for spec in get_constraints() if spec.name != "order_revisit_cost"]
picks = [*others, OrderRevisitCost(weight=10)]
solution = op.build_demo_data(trolleys_count=10, orders_count=24)
config = op.SolverConfig()
config.step_count_limit = 3_000_000
config.random_seed = 1
op.solve(solution, config=config, constraints=picks)
raw = op.constraint_costs(solution, picks)
walk = sum(raw["distance_from_previous_pick_task_cost"]["costs"]) + sum(
    raw["distance_from_last_pick_task_to_path_origin_cost"]["costs"]
)
print(sum(raw["order_revisit_cost"]["costs"]), walk)
```

On the larger demo that prints 51 returns and 1,914 m of walking. The same solve with
the constraint at weight 0 (`constraints=get_constraints()`) leaves 177 returns and walks
1,552 m: at weight 10 the pickers walk 23% more to come back to an order 71% less
often.

What the base gave you: the engine calls `cost()` for the lists a move changes and
for no others, and reports the constraint per trolley in `constraint_costs()`.

## 3. Check it

**A case worked out by hand**, in `tests/order_picking/cpp/op_plugin_test.cpp`, next
to the other cases of `verifyRawCostsByHand()`. They build a solution with
`makeBase()`, `addTrolley()` and `addPick()` and read the raw cost of each trolley
off the default assembly with `rawCosts()`:

```cpp
    {
        // Orders 0 1 0 1 0 on the first trolley: the third, fourth and fifth
        // picks each come back to an order the trolley left. Orders 0 0 1 on
        // the second: none does.
        OrderPickingSolution s = makeBase(2);
        const int t1 = addTrolley(s, 10, 100, origin);
        const int t2 = addTrolley(s, 10, 100, origin);
        for (int order : {0, 1, 0, 1, 0})
            s.trolleys[t1].pickTaskIndices.push_back(addPick(s, order, 1, origin));
        for (int order : {0, 0, 1})
            s.trolleys[t2].pickTaskIndices.push_back(addPick(s, order, 1, origin));
        check(rawCosts(s, "orderRevisitCost") == std::vector<double>{3.0, 0.0},
              "orderRevisitCost: [3, 0] (0 1 0 1 0; 0 0 1)");
    }
```

```sh
cmake -B build -DOPTIMARU_USE_CASES=order_picking && cmake --build build -j
ctest --test-dir build --output-on-failure     # op_test, op_plugin_test
uv run pytest -q tests/order_picking tests/test_constraints_contract.py
```

**The counts.** A fifth constraint fails two checks of `op_plugin_test` and three Python
tests. Update them:

- `op_plugin_test.cpp`: `constraint_totals().size() == 4` in
  `verifyConstraintTotals()`, and `rows.size() == 4` in `verifyCatalog()`. `rowIs()`
  there expects a constraint that is on by default, so check the new row's fields directly
  (`rows[4].name == "orderRevisitCost"`, `!rows[4].default_enabled`, ...);
- `tests/order_picking/test_constraint_specs.py`: the lists in
  `test_list_constraints_matches_the_spec_names`,
  `test_default_instance_carries_no_overrides` (the module-level instances, so the new
  one is imported there) and `test_constraint_info_reports_the_cpp_defaults`.

**Incremental against full.** Solve with every score checked against a full recompute, and every
preview of a move against the score after the move. It raises `RuntimeError` ("Score corruption:
...", the engine's `ScoreMismatchError`) if they differ:

```python
op.solve(solution, time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

`op_plugin_test` does the same for the default assembly, which now holds the constraint,
but at weight 0, and a constraint at weight 0 is not asked on a move, previewed or applied.
The solve above, with the constraint picked, is what checks it.

**The tables.** The constraint tables in [README.md](README.md) and
[python.md](python.md) must equal `op.constraint_info()`; add the row. The constraints are
also counted or listed in "all four are scored by default" under the README table, in
[cpp.md](cpp.md), in the docstrings of `constraints.py` and `_core.pyi`, in the
comments of `common.hpp` and `constraints.hpp`, and the notebook
`examples/order-picking.ipynb` prints the constraint list.

## 4. Make it cheaper per move

On the assembly a constraint is asked in two ways. Most candidate moves are only
**previewed**: the engine asks every constraint with a weight what the changed lists would
cost (`cost()` on the candidate list, written out once per preview), and a constraint at
weight 0 is not asked at all. A move it accepts is **applied**: the changed lists are
reloaded and `cost()` of every constraint with a weight runs on them. A constraint at weight
0 is left out there too: only a rebuild of the objective (below) costs it, and
`constraint_costs()` when it reports. Counted with a throwaway counter in the example on the
larger demo, 3 million steps: 1.87 calls of `cost()` per step at weight 10, and at weight 0
thirty in the whole solve, all from the three rebuilds. So a constraint left at weight 0 costs a
move nothing, and what a picked constraint costs is what its `cost()` costs:

| The constraint is | Use | Per preview | Example to copy |
| --- | --- | --- | --- |
| a function of the walk along the list | `OpUnitConstraint` with `cost()` over `members` | one pass over each changed list | [`distance_from_previous_pick_task_cost.hpp`](../../../optimaru/order_picking/cpp/include/op/constraints/distance_from_previous_pick_task_cost.hpp) |
| a function of the orders on the list | the same, with a per-order scratch sized in `on_reset()` | one pass, no allocation | [`order_split_by_trolley_cost.hpp`](../../../optimaru/order_picking/cpp/include/op/constraints/order_split_by_trolley_cost.hpp), [`trolley_capacity_cost.hpp`](../../../optimaru/order_picking/cpp/include/op/constraints/trolley_capacity_cost.hpp), the example below |
| a sum along the list that prefix sums can settle | `piecewise_cost()` on sums kept in a shared context | a few array reads per piece | none here; [`vehicle-routing`](../vehicle-routing/extending.md#4-make-it-cheaper-per-move) has them |
| anything across trolleys | `OpGlobalConstraint` with `supports_preview()` and `preview_cost()` | what your preview costs | none here; see below |

A global without `supports_preview()` is correct and expensive: it switches the
previews off for the whole assembly, and every candidate is then applied, scored and
undone, as on the default path. A piecewise answer saves nothing while another constraint
with a weight needs the written-out list, and all four constraints here do. `full_assert`
(step 3) checks every preview against the applied score.

**Compute what you can once per rebuild.** `on_reset()` runs when the objective is
rebuilt. A sequence-shaped objective is also rebuilt when a compound move is rejected
and the lists are put back from a snapshot (`SolutionSnapshot::restore` in the
engine's `model/snapshot.hpp`). The moves that do that (route elimination, ruin and
recreate as a move, the iterated local search) are not used here: `Solver::Config` of
this use case has no switch for them, and the counter saw 3 calls of `on_reset()` in 3
million steps. A table built in `on_reset()` is paid once per solve. Sequence-shaped
use cases that run those moves rebuild far more often; see the
[vehicle-routing guide](../vehicle-routing/extending.md#4-make-it-cheaper-per-move).

**The example.** Step 2 allocates a flag per order in every call. One flag vector,
sized in `on_reset()` and cleared after each call, takes that away (`mutable`, as
`cost()` is `const`, the way `OrderSplitByTrolleyCost` keeps its scratch):

```cpp
    // One flag per order, sized once per rebuild: cost() below then
    // allocates nothing.
    void on_reset(const OrderPickingSolution& solution) override {
        left_.assign(solution.orders.size(), 0);
    }
    double cost(const OrderPickingSolution& solution, int /*unit*/,
                const std::vector<int>& members) const override {
        std::int64_t revisits = 0;
        int previous = -1;
        for (int pickIndex : members) {
            const int order = solution.pickTasks[pickIndex].orderIndex;
            if (order == previous) continue;
            if (left_[order]) ++revisits;
            if (previous >= 0) left_[previous] = 1;
            previous = order;
        }
        for (int pickIndex : members) left_[solution.pickTasks[pickIndex].orderIndex] = 0;
        return static_cast<double>(revisits);
    }

private:
    mutable std::vector<char> left_;  // order picked, then left (scratch, zeroed after use)
```

`cost()` returns what it returned before, and the solves end on the same scores. The
assembly's steps per second at weight 10, the weight of the step-2 solve, in millions,
against 2.20 for the four constraints alone:

| | At weight 10 |
| --- | --- |
| step 2 | 1.68 |
| with the scratch | 1.76 |

At weight 10 the constraint is asked in every preview, and the scratch takes 5% more steps
per second. The distance from 2.20 to 1.76 is not all the constraint: with returns priced
the search accepts other moves. At weight 0 the constraint is asked on no move, with the
scratch or without, and the assembly runs as the four do.

**Scope and whole numbers.** `scoped_units()` returns the units a constraint is ever
non-zero for, and the engine skips it on the others; a constraint here evaluates every
trolley. Return `static_cast<double>` of an integer count: meters, buckets and counts
keep sums exact.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`op.solve(solution)` must score it, and it scores with the monolithic calculators,
the only path with `OrderConsolidateMove` and parallel starts. `op_plugin_test`
compares the default assembly with `ScoreCalculator::score` on random list layouts
(`verifyAssemblyParity()`) and over random list moves (`verifyIncremental()`), so it
fails from the moment the weight is not 0 until both sides agree. `op_test` holds the
incremental calculator to the full recompute.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 10.0;`.
It must be a whole number: the monolithic calculators add in `int64` and read it
through `kSoftWeight<OrderRevisitCost>` (`common.hpp`), which fails to compile on a
fractional weight or another level.

**2. The full recompute**, `cpp/include/op/score_calculator.hpp` and
`cpp/src/score_calculator.cpp`. A member `orderRevisitCost` in `ConstraintTotals`
(named after the registry name, at the default weight), subtracted in `toScore()`;
`kRevisitWeight = kSoftWeight<OrderRevisitCost>` next to the other weights; and the
term at the end of `ScoreCalculator::calculateTotals()`:

```cpp
    // --- soft: orderRevisitCost, picks back on an order the trolley left --
    std::vector<char> left(static_cast<std::size_t>(orderCount), 0);
    for (const Trolley& trolley : solution.trolleys) {
        int previous = -1;
        for (int pickIndex : trolley.pickTaskIndices) {
            const int order = solution.pickTasks[pickIndex].orderIndex;
            if (order == previous) continue;
            if (left[order]) totals.orderRevisitCost += kRevisitWeight;
            if (previous >= 0) left[previous] = 1;
            previous = order;
        }
        std::fill(left.begin(), left.end(), 0);
    }
```

**3. The incremental calculator**, `cpp/include/op/incremental_calculator.hpp` and
`cpp/src/incremental_calculator.cpp`. It keeps each trolley's terms (`shortage_`,
`dist_`) and re-derives them from the list in `after_list_edit()`. The constraint is one
more such term: a `revisit_` per trolley and a `revisitTotal_`, set in `rebuild()`,
taken off in `before_list_edit()`, set again in `after_list_edit()`, and in
`score()`; the walk reuses the order stamps that `requiredBuckets()` already keeps,
so it needs no clearing:

```cpp
// The order stamps mark the orders the walk has left (a new generation per
// call), so no flag needs clearing afterwards.
std::int64_t OrderPickingIncrementalScoreCalculator::revisits(int trolley) {
    ++stampGeneration_;
    std::int64_t count = 0;
    int previous = -1;
    for (int pickIndex : solution_->trolleys[trolley].pickTaskIndices) {
        const int order = solution_->pickTasks[pickIndex].orderIndex;
        if (order == previous) continue;
        if (orderStamp_[order] == stampGeneration_) ++count;
        if (previous >= 0) orderStamp_[previous] = stampGeneration_;
        previous = order;
    }
    return count;
}
```

With the fifth constraint the default path ran at 0.71 million steps per second, where
four constraints ran at 0.86. Part of that is the term, one more walk of each list
the default path applies and undoes; part is the search, which now trades walking
against returns and ends on another plan. The assembly with the constraint at weight 10 ran
at 1.76.

**4. What else moves.** A constraint that is on by default changes every default score.
With the example at weight 10 these failed, and are expected to:

- the catalog checks of step 3 again: the row's weight and `default_enabled` in
  `verifyCatalog()`, and the list in `test_constraint_info_reports_the_cpp_defaults`;
- `test_weight_none_means_the_component_default`, which spells out the default weights
  of the four constraints and so leaves the fifth out. Nothing else failed: `op_test` checks
  each constraint's member of `ConstraintTotals` on its own;
- the constraint tables and the score text ("soft = -(travel distance + 1000 x (order,
  trolley) pairs)") in [README.md](README.md), [python.md](python.md),
  [cpp.md](cpp.md), `constraints.py`, `_core.pyi`, `domain.hpp`,
  `score_calculator.hpp`, `incremental_calculator.hpp`, `common.hpp`,
  `constraints.hpp`, and `CHANGELOG.md`;
- the CP-SAT model, `external_solver.order_picking` in
  [external-solver](https://github.com/optimaru/external-solver): a plugin
  subclassing the spec, defaults from `constraint_info()`. The benchmark,
  [`benchmarks/order-picking`](../../../benchmarks/order-picking), runs the default
  path (`op.solve` without `constraints`) and rescores both answers with this use
  case's evaluator, so without the same constraint in the CP-SAT model it no longer compares
  one objective, and its published results change.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed, on the
larger demo:

```python
import time

import optimaru.order_picking as op
from optimaru.order_picking.constraints import get_constraints

steps = 3_000_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    solution = op.build_demo_data(trolleys_count=10, orders_count=24)  # solve() works in place
    config = op.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    op.solve(solution, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e6:5.2f} M steps/s  {solution.score}")
```

Each line repeats exactly from run to run, but the two lines do not end on the same
score (see [How a plan is scored](#how-a-plan-is-scored)). Each figure below is the
median of three runs of the script, each the median of three solves.

| Measured on the larger demo, in million steps per second | |
| --- | --- |
| default path, four constraints | 0.86 |
| assembly, four constraints | 2.20 |
| assembly, four + the constraint of step 2 at weight 0 | 2.19 |
| assembly, the constraint of step 2 at weight 10 | 1.68 |
| assembly, the constraint with the scratch of step 4, at weight 10 | 1.76 |
| default path, five constraints (step 5) | 0.71 |

A step of the assembly is mostly a preview and a step of the default path an applied
and undone move, so the first two lines compare the paths, not the constraints. The
second and third lines differ by noise only: a constraint at weight 0 is asked on no move.

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
plans at the same budget over several seeds, before and after: the score, and the raw
totals the constraint trades against each other.

```python
for seed in range(1, 6):
    solution = op.build_demo_data(trolleys_count=10, orders_count=24)
    op.solve(solution, time_limit_seconds=10, random_seed=seed, constraints=picks)
    raw = op.constraint_costs(solution, picks)
    print(seed, solution.score, sum(raw["order_revisit_cost"]["costs"]))
```

For a constraint on the default path (step 5), `./build/order_picking/op_demo` solves the
demo warehouse for 10 s on that path: build the tree before and after the change into
two build directories and run the two binaries in turn. Against CP-SAT,
`uv run order-picking-compare --seconds 10 --workers 8` in
`benchmarks/order-picking/cpsat` ([README](../../../benchmarks/order-picking/README.md)).

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/op/constraints/<subject>_cost.hpp` | new | | `on_reset`, scratch | weight |
| `cpp/include/op/constraints.hpp` | include, `emplace` | | | |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | the default in the text |
| `tests/order_picking/cpp/op_plugin_test.cpp` | | case, catalog row, count | | catalog row |
| `tests/order_picking/test_constraint_specs.py` | | lists | | catalog list, default weights |
| `docs/use-cases/order-picking/README.md`, `python.md`, `cpp.md`, `_core.pyi`, the notebook | | table row, counts | | defaults, score text |
| `cpp/include/op/score_calculator.hpp`, `cpp/src/score_calculator.cpp` | | | | `ConstraintTotals` member, the term |
| `cpp/include/op/incremental_calculator.hpp`, `cpp/src/incremental_calculator.cpp` | | | | the term, its total |
| external-solver, `benchmarks/order-picking` | | | | the same constraint, results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

# Container loading: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint that is on in every solve.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its eight constraints are in the [guide](README.md) and [python.md](python.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | one C++ test case, a catalog row, a count, the lists in the tests, the two reference scorers |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header, the decode |
| [5](#5-turn-it-on-by-default) | Turn it on by default | the weight, the full recompute |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `gravityHeightCost`, how high a container's load sits. A
pallet with its heavy cartons on top tips; a van with them on the floor drives better. The
eight constraints say whether a box may stand where it stands and what the containers
cost; none says that of two loadings that fit, the one with the weight low is the better.
Every snippet below was compiled and run, and every figure measured, on optimaru 0.8.0
with this use case added; a later change to the solver moves the figures, not the steps.

## How a loading is scored

An item is given a place in a container's loading order. The score is kept per **unit**:
one container, with the items of its list, in loading order, as its **members**. There is
no pool: after construction every item is in some container's list. A move changes one or
two lists, so only those are scored again.

What sets this use case apart is that most costs are not functions of the list as such.
They cost where the items **stand**, and that is derived: the decoder
([`cl/score/packing.hpp`](../../../optimaru/container_loading/cpp/cl/score/packing.hpp))
places the list's items one after the other and returns a `Packing`, the placed boxes
with their positions. Six of the seven unit constraints read a `Packing`; they share
one decode per container and list through `PackingContext`
([`constraints/common.hpp`](../../../optimaru/container_loading/cpp/cl/constraints/common.hpp)),
so a move that touches a container decodes its candidate list once however many of them
are in the assembly. `container_weight_cost` needs no decode, and `group_split_cost`,
which spans containers, is the one global constraint.

There is one scoring path. `cl.solve(problem)` scores with the default assembly, the same
components a selection picks from, so the two calls below run the same search and return
the same loading (`cl_plugin_test` checks the assembly against a full recompute,
`ScoreCalculator`, on 300 random loadings and over 3,000 random list moves):

| | `cl.solve(problem)` | `cl.solve(problem, constraints=[...])` |
| --- | --- | --- |
| Scored by | the assembly: one component per constraint, `cpp/cl/constraints/` | the same |
| Constraints | the eight, at their defaults (two of them at weight 0) | the ones you list, with your knobs |
| Steps per second | 39.4 thousand | 41.8 thousand with the defaults listed (one search: the difference is noise) |
| Moves | the built-in list moves, `EmptyContainerMove`, `RepackPairMove`, `SwitchContainerMove`, `StopOrderMove` | the same |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are the demo made larger, `cl.generate_demo_data(item_count=120,
container_count=8)` (120 boxes for three stores, 8 pallets), 200,000 steps, one thread of an
Apple M4 Max, the median of seven solves; [step 6](#6-measure) has the script. A step is mostly a **preview**: every
constraint here can say what a candidate list would cost without the move being made
(the global one too), so the engine applies only the moves it accepts. A preview of a
list is a decode of it, from the place where it differs from the list as it is.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import optimaru.container_loading as cl
from optimaru.container_loading.constraints import GroupSplitCost, StackingLoadCost, UsedLengthCost, get_constraints

problem = cl.generate_demo_data(item_count=120, container_count=8)
changed = {"group_split_cost", "stacking_load_cost", "used_length_cost", "load_balance_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    GroupSplitCost(weight=0.2),                    # lighter (default 1): a pallet saved beats an order kept whole
    StackingLoadCost(level="soft", weight=0.001),  # hard by default
    UsedLengthCost(weight=0.0005),                 # opt-in: 0 by default
]                                                  # the balance: dropped
cl.solve(problem, time_limit_seconds=10, constraints=picks)           # solves in place
```

To try weights without solving again, score a finished loading with other picks:

```python
from optimaru.container_loading.constraints import score

raw = cl.constraint_costs(problem, picks)   # raw cost of every constraint, per container
print(score(raw, picks))                    # the (hard, soft) those picks give this loading
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/container_loading/constraints.py).
The domain has two levels: `level="medium"` raises `ValueError`. A compiled problem is
solved as it is given; a pydantic model that sets `max_load_on_top` while the list leaves
`stacking_load_cost` out is refused (see
[Parameters and their costs](README.md#parameters-and-their-costs)). Stop here if the
constraint you need is one of the eight.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are applied by
the engine.

There are two bases in
[`constraints/common.hpp`](../../../optimaru/container_loading/cpp/cl/constraints/common.hpp),
and you write one function, `cost(problem, unit, members)`: the raw cost of container
`unit` holding `members` in that order.

- `detail::PackedConstraint`, for a cost of where the items stand. Its
  `packing(problem, unit, members)` is the decode of that list: `boxes` (the placed
  items in loading order, each with `item`, `x`, `y`, `z`, `length`, `width`, `height`),
  `unplaced()`, `loadedVolume`, `loadedWeight`, `usedLength`.
- `ClUnitConstraint`, for a cost of the list alone, such as `container_weight_cost`.

`cost()` reads the problem's facts and its arguments, and never the derived fields of an
`Item` or a `Container` (`item.x`, `container.placedItems`, ...): those describe the lists
as they were last scored, and a preview asks about lists that do not exist yet. A
constraint that does not decompose per container is a `ClGlobalConstraint`;
[step 4](#4-make-it-cheaper-per-move) says what that costs the assembly.

The example reads one decode, so it is one function. Four edits:

**1. The component**,
`optimaru/container_loading/cpp/cl/constraints/gravity_height_cost.hpp`:

```cpp
#ifndef CL_CONSTRAINTS_GRAVITY_HEIGHT_COST_HPP
#define CL_CONSTRAINTS_GRAVITY_HEIGHT_COST_HPP

#include <cmath>

#include "cl/constraints/common.hpp"

namespace cl {

// Soft, weight 0 (opt-in, default_enabled = false): gravityHeightCost -- how
// high a container's load sits. Raw cost per container = the height of its
// placed items' centre of gravity, in thousandths of the container's height
// (0 = on the floor, 500 = half way up); 0 for a container whose placed items
// weigh nothing. Give it a weight to put the heavy items at the bottom.
struct GravityHeightCost final : detail::PackedConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit GravityHeightCost(double weight = kDefaultWeight)
        : PackedConstraint("gravityHeightCost", weight, kDefaultLevel) {}

    double cost(const ContainerLoadingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        if (members.empty()) return 0.0;
        double weight = 0.0;
        double moment = 0.0;
        for (const Box& box : packing(problem, unit, members).boxes) {
            const double w =
                    static_cast<double>(problem.items[static_cast<std::size_t>(box.item)].weight);
            weight += w;
            moment += w * (static_cast<double>(box.z) + 0.5 * static_cast<double>(box.height));
        }
        if (weight == 0.0) return 0.0;
        const double height =
                static_cast<double>(problem.containers[static_cast<std::size_t>(unit)].height);
        return std::floor(1000.0 * moment / (weight * height) + 0.5);
    }
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_GRAVITY_HEIGHT_COST_HPP
```

The name passed to the base, `"gravityHeightCost"`, is the struct name with a lower-case
first letter. Python derives `gravity_height_cost` from it.

The defaults live in these two constants and nowhere else; Python reads them through
`cl.constraint_info()`. `kDefaultLevel` is `Hard` or `Soft` (the score is the engine's
`Score` as it is), and the weight is in units of that level. A hard constraint returns
whole numbers, because a hard contribution is rounded per container; the example rounds to
whole thousandths so that it could be flipped to hard.

**2. The assembly**, `cpp/cl/constraints.hpp`: include the header, and add one line to
`defaultConstraints()`, after `LoadBalanceCost` (the unit constraints come before the
global one in every list):

```cpp
    assembly.emplace<GravityHeightCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a weight,
so no solve changes.

**3. The Python spec**, `optimaru/container_loading/constraints.py`:

```python
class GravityHeightCost(Constraint):
    """How high each load's centre of gravity sits. Default: soft, weight 0 (opt-in)."""

    name = "gravity_height_cost"
    label_en = "Height of the centre of gravity"
    label_ko = "무게중심 높이"
    description_en = (
        "For each container, the height of its load's centre of gravity in thousandths of the "
        "container's height (0 = on the floor, 500 = half way up). 0 for a container whose "
        "placed items weigh nothing. Off until it is given a weight."
    )
    description_ko = (
        "컨테이너마다 적재물 무게중심의 높이를 컨테이너 높이의 천분율로 더합니다(0 = 바닥, "
        "500 = 절반 높이). 놓인 물품의 무게가 0이면 0입니다. 가중치를 주기 전에는 꺼져 있습니다."
    )
```

**4. Its default instance**, in the same file: `gravity_height_cost = GravityHeightCost()`
with the other instances, that instance in `_DEFAULTS` after `load_balance_cost` (the
`list_constraints()` order), and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=container_loading
uv sync --reinstall-package optimaru       # rebuilds optimaru.container_loading._core
```

```python
from optimaru.container_loading.constraints import GravityHeightCost

others = [spec for spec in get_constraints() if spec.name != "gravity_height_cost"]
picks = [*others, GravityHeightCost(weight=0.01)]
problem = cl.generate_demo_data(item_count=120, container_count=8)
config = cl.SolverConfig()
config.stop.step_limit = 200_000
config.run.random_seed = 1
cl.solve(problem, config=config, constraints=picks)
raw = cl.constraint_costs(problem, picks)
print(problem.score, problem.containers_used(), raw["gravity_height_cost"]["costs"])
```

On the larger demo that prints `0hard/-32.48soft`, 5 pallets, and on those five heights of
350, 351, 396, 353 and 398 thousandths: 370 on average. The same solve with the constraint
at weight 0 (`constraints=get_constraints()`) ends on `0hard/-9soft`, 6 pallets, with the
centres of gravity at 464, 433, 468, 434, 404 and 428: 439 on average. At weight 0.01 the
loads sit 16% lower and pay for it in store splits: 32.48 is 0.01 x 1,848 for the heights
plus 14 for pallets and splits, where the other solve has 9. A weight is a price, and
this one is high: a hundredth per thousandth makes a tenth of the height worth a pallet.

What the base gave you: the engine calls `cost()` for the lists a move changes and for no
others, the decode behind `packing()` is made once and shared with the other constraints,
and the constraint is reported per container in `constraint_costs()`.

## 3. Check it

**A case worked out by hand**, in `tests/container_loading/cpp/cl_plugin_test.cpp`, next
to the other cases of `verifyRawCostsByHand()`. They build a problem with
`addContainer()` and `addItem()` and read the raw cost of each container off the default
assembly with `rawCosts()`:

```cpp
    {
        // gravityHeightCost: a column of two boxes of 4 x 4 x 4 in a space 16
        // high. The heavy one (30) below the light one (10): the centre of
        // gravity is at (30 * 2 + 10 * 6) / 40 = 3, 188 thousandths of 16. The
        // other way round: (10 * 2 + 30 * 6) / 40 = 5, 313 thousandths.
        ContainerLoadingProblem p;
        addContainer(p, 4, 4, 16);
        addContainer(p, 4, 4, 16);
        const int heavy = addItem(p, 4, 4, 4, 30);
        const int light = addItem(p, 4, 4, 4, 10);
        p.containers[0].items = {heavy, light};
        check(rawCosts(p, "gravityHeightCost") == std::vector<double>({188.0, 0.0}),
              "gravityHeightCost: the heavy box below; 0 for an empty container");
        p.containers[0].items = {light, heavy};
        check(rawCosts(p, "gravityHeightCost") == std::vector<double>({313.0, 0.0}),
              "gravityHeightCost: higher with the heavy box on top");
    }
```

```sh
cmake -B build -DOPTIMARU_USE_CASES=container_loading && cmake --build build -j
ctest --test-dir build --output-on-failure     # cl_test, cl_plugin_test
uv run pytest -q tests/container_loading tests/test_constraints_contract.py
```

**The counts.** A ninth constraint fails three checks of `cl_plugin_test` and
eight Python tests. Update them:

- `cl_plugin_test.cpp`: `rows.size() == 8` in both checks of `verifyCatalog()` (add a
  `rowIs<GravityHeightCost>(rows[7], "gravityHeightCost", soft, 0.0, false)` and move the
  global one to `rows[8]`), and `constraint_totals().size() == 8` in
  `verifyConstraintTotals()`;
- `tests/container_loading/test_constraint_specs.py`: `NAMES` and `CLASSES` at the top (they
  feed `test_list_constraints_matches_the_spec_names`), the rows of
  `test_constraint_info_reports_the_cpp_defaults`, and the raw costs of
  `test_constraint_costs_are_the_raw_costs_by_hand`;
- the two reference scorers, which recompute every constraint from the positions and
  compare the whole dict with the engine's: `raw_costs()` in
  `tests/container_loading/reference_container_loading.py`
  (`test_random_loadings_are_valid_and_cost_what_the_reference_says`,
  `test_solved_problems_are_valid_and_cost_what_the_reference_says` and
  `test_the_demo_and_the_fixture_are_valid_loadings` fail until it has the new cost) and
  `raw_costs()` in `benchmarks/container-loading/cpsat/container_loading_cpsat/evaluator`
  (`test_the_evaluator_scores_the_engines_answer_as_the_engine_does` there). Writing the
  cost a second time, from the positions and not from the `Packing`, is the check that the
  component computes what its comment says;
- `tests/test_layout.py`: `test_a_guide_states_the_number_of_constraints_its_use_case_has`
  reads "eight" in the first paragraph of this guide.

**Incremental against full.** Solve with every score checked against a full recompute, and
every preview of a move against the score after the move. It raises `RuntimeError`
("Score corruption: ...", the engine's `ScoreMismatchError`) if they differ:

```python
cl.solve(problem, time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

`cl_plugin_test` does the same for the default assembly, which now holds the constraint,
but at weight 0, and a unit constraint at weight 0 is not asked on a move, previewed or
applied. The solve above, with the constraint picked, is what checks it; to keep that
check in the suite, add the constraint with a weight to `weightedAssembly()` in
`cl_plugin_test.cpp`, which `verifyPreviews()` and `verifyFullAssertSolve()` run.

**The tables.** The constraint tables in [README.md](README.md) and
[python.md](python.md) must equal `cl.constraint_info()`; add the row. The constraints are
also counted or listed in "six of the eight are scored by default" under the README
table, in the docstrings of `constraints.py`, `__init__.py` and `_core.pyi`, in the
comments of `common.hpp` and `constraints.hpp`, in the first paragraph of this guide, and
the notebook `examples/container-loading.ipynb` prints the constraint list.

## 4. Make it cheaper per move

A constraint is asked in two ways. Most candidate moves are only **previewed**: the engine
asks every constraint with a weight what the changed lists would cost (`cost()` on the
candidate list, written out once per preview), and a constraint at weight 0 is not asked
at all. A move it accepts is **applied**: the changed lists are reloaded and `cost()` of
every constraint with a weight runs on them. A constraint at weight 0 is left out there
too: only a rebuild of the objective costs it, and `constraint_costs()` when it reports.

Here the expensive part of a `cost()` is not yours. It is the decode, and the decode is
shared:

| The constraint is | Use | Per preview | Example to copy |
| --- | --- | --- | --- |
| a function of the list alone (weights, counts) | `ClUnitConstraint` with `cost()` over `members` | one pass over each changed list, no decode | [`container_weight_cost.hpp`](../../../optimaru/container_loading/cpp/cl/constraints/container_weight_cost.hpp) |
| a function of where the items stand | `detail::PackedConstraint`, `packing()` in `cost()` | the decode, once for all such constraints, plus your pass over the boxes | [`used_length_cost.hpp`](../../../optimaru/container_loading/cpp/cl/constraints/used_length_cost.hpp), the example |
| the same, and not linear in the boxes (pairs, loads passed down) | a function next to `decode()` that keeps its result with the `Packing` | the decode, plus your function once per decode | [`stacking_load_cost.hpp`](../../../optimaru/container_loading/cpp/cl/constraints/stacking_load_cost.hpp) with `stackingExcess()` in `packing.cpp` |
| anything across containers | `ClGlobalConstraint` with `supports_preview()` and `preview_cost()` | what your preview costs | [`group_split_cost.hpp`](../../../optimaru/container_loading/cpp/cl/constraints/group_split_cost.hpp) |

**What the decode costs, and who pays.** The first `PackedConstraint` asked about a list
pays for its decode; every other one finds it in the cache. In the default set
`unplaced_item_cost` and `used_container_cost` always ask, so a further
`PackedConstraint` adds no decode at all: only its own pass over the boxes. And a move
that is accepted after its preview costs no second decode: the list it becomes is the
candidate that was just decoded. A list is decoded from the place where it differs from
the container's current list, so a change near the end of a loading order is cheaper to
judge than one at its start.

**Keep it with the decode.** The same list is costed several times: by the preview, again
when the move is applied, and by every pick of a constraint that is picked twice. A cost
that is more than a pass over the boxes is computed once per decode and kept in the
`Packing`: a `mutable` field that `decode()` resets, filled by a function in
`packing.cpp` on first use (`stackingExcess()`, `unloadingBlocks()` and
`balancePerMille()` are written this way). For the example that is three edits: a field
`mutable int gravityPerMille = -1;` in `Packing`, `out.gravityPerMille = -1;` at the end
of `decode()`, and the function, declared in `packing.hpp`:

```cpp
int gravityPerMille(const ContainerLoadingProblem& problem, int container, const Packing& packing) {
    if (packing.gravityPerMille >= 0) return packing.gravityPerMille;
    double weight = 0.0;
    double moment = 0.0;
    for (const Box& box : packing.boxes) {
        const double w = static_cast<double>(problem.items[static_cast<std::size_t>(box.item)].weight);
        weight += w;
        moment += w * (static_cast<double>(box.z) + 0.5 * static_cast<double>(box.height));
    }
    int result = 0;
    if (weight > 0.0) {
        const double height =
                static_cast<double>(problem.containers[static_cast<std::size_t>(container)].height);
        result = static_cast<int>(std::floor(1000.0 * moment / (weight * height) + 0.5));
    }
    packing.gravityPerMille = result;
    return result;
}
```

with `cost()` reduced to
`return static_cast<double>(gravityPerMille(problem, unit, packing(problem, unit, members)));`.
A fact that a kept cost reads and the decode does not (here none: the weights are already
among them) also goes into `factsOf()` in `packing.cpp`, which decides whether kept
decodes are still valid for the problem.

`cost()` returns what it returned before, and the solves end on the same scores. Steps per
second at weight 0.01, the weight of the step-2 solve, in thousands, against
39.4 to 41.8 for the eight constraints alone:

| | At weight 0.01 |
| --- | --- |
| step 2 | 40.9 |
| kept with the decode | 42.1 |

The two rows differ by less than two runs of one row do on the machine these were measured
on, and neither can be told from the eight constraints alone: the pass over the twenty
boxes of a pallet is small next to their decode, which the constraint gets for nothing.
Keeping a result with the decode pays for a cost that is quadratic in the boxes, as the
stacking loads are. At weight 0 the constraint is asked on no move, kept or not.

**A global constraint must preview.** A global without `supports_preview()` is correct and
expensive: it switches the previews off for the whole assembly, and every candidate is
then applied, scored and undone, which here means decoding every candidate twice.
`GroupSplitCost` shows a preview that costs what the changed lists hold, not every group.
`full_assert` (step 3) checks every preview against the applied score.

**Whole numbers.** Return `static_cast<double>` of an integer where the cost is one:
counts, lengths and weights keep sums exact, and a hard cost is rounded per container
anyway.

## 5. Turn it on by default

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). There is
no second scoring path to write: `cl.solve(problem)` scores with the default assembly,
which already holds the constraint. What has to follow is everything that states the
default score.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 0.01;`.

**2. The full recompute**, `cpp/cl/score/score_calculator.hpp` and
`cpp/cl/score/score_calculator.cpp`. `ScoreCalculator::score` is what scores the loading a
default solve returns, and what `cl_plugin_test` holds the assembly to on random loadings
(`verifyAssemblyParity()`) and over random list moves (`verifyIncremental()`), so both
fail from the moment the weight is not 0 until the two agree. A member `gravityHeightCost`
in `ConstraintTotals` (named after the registry name, the raw cost summed over the
containers), its term in `toScore()`,

```cpp
                        GravityHeightCost::kDefaultWeight * static_cast<double>(gravityHeightCost);
```

and one line in the loop of `ScoreCalculator::calculateTotals()`:

```cpp
        totals.gravityHeightCost += gravityPerMille(problem, unit, packing);
```

**3. What else moves.** A constraint that is on by default changes every default score.
With the example at weight 0.01 these failed, and are expected to:

- the catalog checks of step 3 again: the row's weight and `default_enabled` in
  `verifyCatalog()`, and the list in `test_constraint_info_reports_the_cpp_defaults`;
- every score worked out by hand on the defaults: five of the six cases of
  `verifyOverrides()` in `cl_plugin_test` (not `only()`, which names one constraint), the
  hand score of `verifyScoreCalculator()` and of the split-load case of `verifyMoves()` in
  `cl_test`; in Python `test_the_default_score_by_hand`,
  `test_picking_a_subset_drops_the_other_terms`,
  `test_reweighting_one_constraint_scales_only_that_term` and
  `test_the_opt_in_costs_score_once_they_have_a_weight` (they build on `get_constraints()`),
  `test_an_item_without_a_place_has_no_position`,
  `test_without_the_parameters_no_cost_is_needed`,
  `test_the_cheapest_cartons_that_hold_an_order_are_chosen`,
  `test_the_van_is_loaded_last_stop_first` and
  `test_the_demo_is_feasible_and_every_box_stands_somewhere`;
- two tests that compare the last score the search reported with the score of the
  returned loading, exactly: `test_on_new_best_reports_step_and_score` and the contract
  test `test_solve_stops_at_the_first_feasible_solution_and_reports_it`. This one is
  the weight's doing, not the constraint's: at 0.01 the soft score is a sum of fractions,
  which the search keeps as a running sum and the full recompute adds up afresh, so the
  two differ in the last bits. Every default weight in the engine's use cases is a whole
  number for that reason. Keep it so: count the raw cost in a coarser unit (hundredths of
  the height at weight 1, say) instead of giving a fine one a fractional weight;
- the constraint tables and the score text in [README.md](README.md),
  [python.md](python.md), `constraints.py`, `__init__.py`, `_core.pyi`, `domain.hpp`,
  `common.hpp`, `constraints.hpp` and `CHANGELOG.md`, and the notebook;
- the benchmark: `test_the_model_covers_weight_limits_costs_and_groups` works a default
  score out by hand, and the CP-SAT model in
  `benchmarks/container-loading/cpsat/container_loading_cpsat/model.py` has the default
  objective written out. A cost of where the weight sits needs a product per item there
  (weight times height, the orientation being a variable), or the model has to refuse
  what it does not cover, as it does for the stacking loads; either way
  [`benchmarks/container-loading`](../../../benchmarks/container-loading) no longer
  compares one objective until it follows, and its published results change.

With the ninth constraint on, the default solve ran at 41.7 thousand steps per second and
ended where the picked one of step 4 did: it is the same assembly.

## 6. Measure

**Steps per second.** The same number of steps, with one seed, on the larger demo:

```python
import time

import optimaru.container_loading as cl
from optimaru.container_loading.constraints import get_constraints

steps = 200_000
for label, picked in (("default", None), ("the defaults, listed", get_constraints())):
    problem = cl.generate_demo_data(item_count=120, container_count=8)  # solve() works in place
    config = cl.SolverConfig()
    config.stop.step_limit = steps
    config.stop.time_limit_seconds = 600
    config.run.random_seed = 1
    start = time.perf_counter()
    cl.solve(problem, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:22s} {steps / seconds / 1e3:6.1f} k steps/s  {problem.score}")
```

Each line ends on the same loading from run to run, and the two lines end on the same
score: it is one scoring path. Each figure below is the median of seven solves.

| Measured on the larger demo, in thousand steps per second | |
| --- | --- |
| default, eight constraints | 39.4 |
| the eight, listed | 41.8 |
| the eight + the constraint of step 2 at weight 0 | 40.1 |
| the constraint of step 2 at weight 0.01 | 40.9 |
| the constraint kept with the decode (step 4), at weight 0.01 | 42.1 |
| default, nine constraints (step 5) | 41.7 |

The first two lines are one search, so their distance is the noise of the measurement
(the machine was not idle): about two thousand steps per second. Every other line is
within that of them. A constraint at weight 0 is asked on no move, and one that reads the
shared decode adds a pass over the boxes to a step that has just decoded them.

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
loadings at the same budget over several seeds, before and after: the score, the
containers used, and the raw totals the constraint trades against each other.

```python
for seed in range(1, 6):
    problem = cl.generate_demo_data(item_count=120, container_count=8)
    cl.solve(problem, time_limit_seconds=10, random_seed=seed, constraints=picks)
    raw = cl.constraint_costs(problem, picks)
    print(seed, problem.score, problem.containers_used(), raw["gravity_height_cost"]["costs"])
```

Where the fewest containers is known,
`uv run container-loading-known-optimum --seconds 10` in
`benchmarks/container-loading/cpsat` runs the engine on containers cut into boxes, and
`uv run container-loading-compare --seconds 10 --workers 8` compares it with CP-SAT
([README](../../../benchmarks/container-loading/README.md)). `./build/container_loading/cl_demo`
solves the demo for 5 s.

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/cl/constraints/<subject>_cost.hpp` | new | | `cost()` calls the kept function | weight |
| `cpp/cl/constraints.hpp` | include, `emplace` | | | |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | the default in the text |
| `cpp/cl/score/packing.hpp`, `cpp/cl/score/packing.cpp` | | | the field, its reset, the function | |
| `tests/container_loading/cpp/cl_plugin_test.cpp` | | case, catalog row, counts | | catalog row |
| `tests/container_loading/test_constraint_specs.py` | | lists | | catalog list, hand scores |
| `tests/container_loading/reference_container_loading.py`, `benchmarks/container-loading/cpsat/container_loading_cpsat/evaluator` | | the cost, from the positions | | |
| `docs/use-cases/container-loading/README.md`, `python.md`, `_core.pyi`, the notebook | | table row, counts | | defaults, score text |
| `cpp/cl/score/score_calculator.hpp`, `cpp/cl/score/score_calculator.cpp` | | | | `ConstraintTotals` member, the term |
| `benchmarks/container-loading` | | | | the model's objective, results |

Moves, the construction and the placement rule are a different subject: see
[Extending](README.md#extending) in the guide.

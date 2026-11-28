# Box sizing: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its eight constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | two C++ test cases, four checks that count or list the catalog |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the two monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `looseFitCost`, the number of shipments that leave in a box
less than half full. Those are the ones that need filler and that a customer notices, and
`voidVolumeCost` does not see them: it adds up litres, so a thousand shipments a little too
loose weigh the same as a hundred that rattle. Every snippet below was compiled and run,
and every figure measured, on the checkout that added this use case (after optimaru 0.8.0).
A later change to the solver moves the figures, not the steps.

## How an assignment is scored

A parcel is assigned a box. The score is kept per **unit**: one candidate box, with the
parcels that ship in it as its **members**. A parcel without a box (`-1`) is in no unit and
scores nothing. A change move touches one parcel; the moves on the set of sizes
(`CloseBoxMove`, `OpenBoxMove`, `SwapBoxMove`) move many parcels in one batch, so many
members leave and join in one step.

There are two scoring paths, and they give the same score for the same assignment
(`bs_plugin_test` checks it on 900 random assignments):

| | `bs.solve(problem)` | `bs.solve(problem, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/bs/score/score_calculator.cpp` and `cpp/bs/score/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/bs/constraints/` |
| Constraints | the eight, at their defaults (so five score) | the ones you list, with your knobs |
| Steps per second | 333 thousand | 163 thousand |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are the instance of step 1 (1,000 parcel sizes, 24 candidate boxes, six box
types), a million steps, one thread of an Apple M4 Max; [step 6](#6-measure) has the script.
Steps 1 to 4 work on the assembly. Step 5 moves a constraint to the monolithic calculators.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import optimaru.box_sizing as bs
from optimaru.box_sizing.constraints import BoxTypeCost, BoxWeightCost, VoidVolumeCost, get_constraints


def build_problem(parcel_count=1000, seed=2):
    """The demo warehouse with more parcel sizes: 24 candidate boxes, six box types."""
    return bs.generate_demo_data(seed=seed, parcel_count=parcel_count).to_core()


problem = build_problem()
changed = {
    "box_weight_cost": BoxWeightCost(level="soft", weight=1000),   # hard by default
    "void_volume_cost": VoidVolumeCost(exponent=1.1),              # shaped per box (default: linear)
    "box_type_cost": BoxTypeCost(weight=5_000_000),                # off by default: 5 litres per size
}
picks = [changed.get(spec.name, spec) for spec in get_constraints()]   # one you leave out is not scored
bs.solve(problem, time_limit_seconds=10, constraints=picks)
```

`bs.solve()` solves a `BoxSizingProblem` in place, and a plain solve ignores the boxes the
parcels hold (`warm_start` makes it start from them). To try weights without solving again,
score a finished assignment with other picks:

```python
from optimaru.box_sizing.constraints import score

raw = bs.constraint_costs(problem, picks)  # raw cost of every constraint: per box, or one count
print(score(raw, picks))                   # the score those picks give this assignment
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/box_sizing/constraints.py).
Stop here if the constraint you need is one of the eight: `box_type_cost` already is the fixed
cost of a size, `box_cost` the price of a box, `board_area_cost` the board it is made of and
`shipping_cost` the freight by billed weight.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

[`constraints/common.hpp`](../../../optimaru/box_sizing/cpp/bs/constraints/common.hpp)
has the bases:

| The constraint is | Base | You write |
| --- | --- | --- |
| a sum over a box's parcels of one number per parcel and box | `detail::PerParcelConstraint` | `term(problem, parcel, box)` |
| another function of one box and its parcels (a clamp on a total, a count) | `BsUnitConstraint` | `cost(problem, unit, members)` |
| a number over the boxes in use | `detail::BoxTypeCount` | `cost()`, from the count it keeps |
| any other statistic across boxes | `BsGlobalConstraint` | `reset()`, `on_unit_changed()`, `cost()` |

Four of the eight constraints that exist are sums of one term per parcel, and so is the
example: one function. Four edits:

**1. The component**, `optimaru/box_sizing/cpp/bs/constraints/loose_fit_cost.hpp`:

```cpp
#ifndef BS_CONSTRAINTS_LOOSE_FIT_COST_HPP
#define BS_CONSTRAINTS_LOOSE_FIT_COST_HPP

#include "bs/constraints/common.hpp"

namespace bs {

// Soft, off by default (weight 0): looseFitCost -- the shipments that leave
// in a box less than half full: the ones that need filler, and that a customer
// notices. Raw cost per box = over its parcels, the quantity of those whose
// volume is below half the box's; 0 = every shipment fills at least half of
// its box.
struct LooseFitCost final : detail::PerParcelConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit LooseFitCost(double weight = kDefaultWeight)
        : PerParcelConstraint("looseFitCost", weight, kDefaultLevel) {}
    double term(const BoxSizingProblem& problem, int parcel, int box) const override {
        const bool loose = 2 * problem.parcelVolume[parcel] < problem.boxVolume[box];
        return loose ? static_cast<double>(problem.parcels[parcel].quantity) : 0.0;
    }
};

}  // namespace bs

#endif  // BS_CONSTRAINTS_LOOSE_FIT_COST_HPP
```

The name passed to the base, `"looseFitCost"`, is the struct name with a
lower-case first letter. Python derives `loose_fit_cost` from it.
`problem.parcelVolume` and `problem.boxVolume` are two of the tables
`ScoreCalculator::prepare()` builds once per problem (the sorted sides and the volumes);
the base's `on_reset()` checks that it ran.

The half is a constant here to keep the example to one function. A fact a caller sets
belongs on the problem: a field of `BoxSizingProblem` in `model/domain.hpp`, read in
`bindings.cpp` and carried through `models.py`, with its row in `parameter_uses()` when
it acts only through this cost.

The defaults live in the two constants and nowhere else; Python reads them through
`bs.constraint_info()`. `kDefaultLevel` is `Hard` or `Soft`: this use case has no
medium level, and a pick with `level="medium"` is refused. A hard constraint returns
whole numbers, because a hard contribution is rounded per box.

**2. The assembly**, `cpp/bs/constraints.hpp`: include the header, add its
row to the comment that lists the set, and add one line to `defaultConstraints()`, after
`ShippingCost` (the unit constraints come before the two globals in the catalog):

```cpp
    assembly.emplace<LooseFitCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no default solve changes and the monolithic calculators stay as they are.

**3. The Python spec**, `optimaru/box_sizing/constraints.py`:

```python
class LooseFitCost(Constraint):
    """The shipments that leave in a box less than half full. Opt-in: soft, weight 0."""

    name = "loose_fit_cost"
    label_en = "Loose fit"
    label_ko = "헐거운 포장"
    description_en = (
        "For each box, over the parcels that ship in it: the quantity of those whose volume is below half "
        "the box's. 0 means every shipment fills at least half of its box. Opt-in: the default weight is 0."
    )
    description_ko = (
        "상자마다 그 상자로 나가는 화물 가운데 부피가 상자의 절반에 못 미치는 화물의 수량을 모두 더합니다. "
        "0이면 모든 출고가 상자의 절반 이상을 채웁니다. 기본 가중치가 0이므로 가중치를 주어야 점수에 반영됩니다."
    )
```

**4. Its default instance**, in the same file: `loose_fit_cost =
LooseFitCost()` with the other instances, that instance in `_DEFAULTS` after
`box_cost` (the order of `list_constraints()`), and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=box_sizing
uv sync                                    # rebuilds optimaru.box_sizing._core
```

```python
from optimaru.box_sizing.constraints import LooseFitCost, get_constraints

LITRES = 1_000_000                         # mm3
picks = [                                  # a loose shipment counts as ten litres of air
    LooseFitCost(weight=10 * LITRES) if spec.name == "loose_fit_cost" else spec for spec in get_constraints()
]
config = bs.SolverConfig()
config.stop.step_limit = 200_000
config.run.random_seed = 1
problem = build_problem()
bs.solve(problem, config=config, constraints=picks)
print(sum(bs.constraint_costs(problem, picks)["loose_fit_cost"]["costs"]), problem.void_volume())
```

On the instance of step 1, 2,615 of the 3,424 shipments are loose and the plan ships
20,976 litres of air. A default solve of the same length (`constraints=None`) leaves 2,752
loose and ships 20,055 litres: the weight bought 137 shipments for 4.6% more air, by
trading the smallest of the six sizes for another. At a tenth of that weight the two solves are
the same; the weight says where the trade starts.

The construction and the moves on the set of sizes do not know the new constraint: they
price a size by its volume and its `cost` (`Pricing`, `model/pricing.hpp`), and the
acceptor judges each step by the whole score, the new constraint included. That is enough
for a constraint that mostly pulls the same way as the volume, as this one does. One that
pulls another way (a size that is good for a reason the volume does not show) is worth a
term in `BoxRanking::unitPrice`, or a move of its own.

What the engine does with `term()`: `PerParcelConstraint` turns it into the raw cost of
a box, the sum over its parcels. The engine keeps that cost for every box, and when
parcels leave or join a box it asks for the box again with its new members: one pass over
the parcels of the box.

## 3. Check it

**A case worked out by hand**, in `tests/box_sizing/cpp/bs_plugin_test.cpp`,
with the other raw costs in `verifyRawCostsByHand()`. Every constraint there has a case
that costs and a case that costs 0:

```cpp
    {
        // B0 holds 6,000,000 mm3. Four shipments of 1,000,000 fill a sixth of
        // it: loose. One of 3,000,000 fills half: not loose. In B1 (24,000,000)
        // seven shipments of 12,000,000 fill half.
        BoxSizingProblem p = twoBoxes();
        addParcel(p, 100, 100, 100, 0, /*quantity*/ 4);
        addParcel(p, 300, 100, 100, 0, /*quantity*/ 1);
        addParcel(p, 400, 300, 100, 1, /*quantity*/ 7);
        check(rawCosts(p, "looseFitCost") == std::vector<double>{4.0, 0.0},
              "looseFitCost: four shipments fill less than half of B0; none in B1");
        p.parcels[0].box = -1;
        check(rawCosts(p, "looseFitCost") == std::vector<double>{0.0, 0.0},
              "looseFitCost: every shipment fills at least half of its box");
    }
```

```sh
cmake -B build -DOPTIMARU_USE_CASES=box_sizing && cmake --build build -j
ctest --test-dir build --output-on-failure
```

**The counts.** A ninth constraint fails four checks that count or list the catalog.
Update them:

- `bs_plugin_test.cpp`: `rows.size() == 8` in `verifyCatalog()`. Add the new row there
  (`rowIs<LooseFitCost>(rows[6], "looseFitCost", soft, 0.0, false)`), and move the two
  globals down one;
- `tests/box_sizing/test_constraint_specs.py`: `NAMES` at the top, which
  `test_list_constraints_matches_the_spec_names` and
  `test_constraint_info_reports_the_cpp_defaults` read (add the row
  `("soft", 0.0, True, False, False)` to the second), and
  `test_module_level_instances_keep_every_default` (the instances).

**Incremental against full.** Solve with every score checked against a full
recompute. It raises `RuntimeError` ("Score corruption: incremental score ... !=
full recompute ...") if a move leaves the score stale:

```python
config = bs.SolverConfig()
config.stop.step_limit = 5_000
bs.solve(build_problem(200), config=config, random_seed=1, constraints=picks, full_assert=True)
```

Pick the constraint with a weight for this, as `picks` does: at weight 0 it adds 0
whatever its raw cost is. To keep the check, add the constraint to the picks of
`test_full_assert_solves_with_repeats_and_offsets`.

**The tables.** The constraint tables in [README.md](README.md) and
[python.md](python.md) must equal `bs.constraint_info()`; add the row. The
docstrings of `list_constraints()` and `constraint_info()` in `_core.pyi` list the
constraints, and the notebook `examples/box-sizing.ipynb` prints them.

## 4. Make it cheaper per move

On the assembly a step costs, for every constraint, something for every box that parcels
leave or join. What that is depends on the constraint:

| The constraint is | Use | Per step | Example to copy |
| --- | --- | --- | --- |
| a sum of one term per parcel | `detail::PerParcelConstraint`: `term()` | one pass over the parcels of each touched box, when the step ends | [`void_volume_cost.hpp`](../../../optimaru/box_sizing/cpp/bs/constraints/void_volume_cost.hpp) |
| another function of the whole box | `BsUnitConstraint` with `cost()` | the same | [`box_cost.hpp`](../../../optimaru/box_sizing/cpp/bs/constraints/box_cost.hpp) |
| the same sum, stating what one parcel adds | add `supports_delta()` and `delta_cost()` | one term per parcel that moves | none here, see below; facility location's [`distance_from_facility_cost.hpp`](../../../optimaru/facility_location/cpp/fl/constraints/distance_from_facility_cost.hpp) |
| a statistic across boxes | `BsGlobalConstraint` with `on_member_changed()`: keep your own totals | what your update costs | `BoxTypeCount` in [`common.hpp`](../../../optimaru/box_sizing/cpp/bs/constraints/common.hpp) |

The example is the first row, and so are four of the constraints that exist. The third
row looks like the cheaper one: a sum of per-parcel terms can state what one parcel adds,
and a change of one parcel then costs one term instead of a pass over its box.

```cpp
    // What one parcel adds to its box, whoever else is there.
    bool supports_delta() const override { return true; }
    double delta_cost(const BoxSizingProblem& problem, int unit,
                      const std::vector<int>& /*members*/, int entity) const override {
        return term(problem, entity, unit);
    }
```

`delta_cost()` must equal `cost(with the parcel) - cost(without it)` exactly; the
`full_assert` solve of step 3 checks that it does, and a seeded solve ends on the same
score as before, as it must.

**Measure before you keep it.** With the constraint at the weight of step 2, on the
instance of step 1:

| | Thousand steps per second |
| --- | ---: |
| `term()` alone | 136 |
| with `delta_cost()` | 124 |

The delta is 9% slower, and that is why the bases here do not have one. Three steps in
four of this search are moves on the set of sizes, which move hundreds of parcels in one
batch. A constraint with `cost()` alone is asked once per touched box when the batch ends,
and when the move is turned down the engine puts back the costs it saved and asks nothing.
A delta is asked for every parcel that moves, and again for each on the way back (see
[`decomposed_objective.hpp`](../../../core/optimaru/score/decomposed_objective.hpp)). The
same holds for the constraints that exist: with a delta on the four per-box constraints,
the default assembly ran at 151 thousand steps per second at best where it runs at 163
thousand. Where steps move one entity at a time and a unit holds hundreds, the delta
wins: facility location's guide measures both cases.

Three more things decide what a constraint costs.

**Compute once per solve.** The facts never change during a solve.
`ScoreCalculator::prepare()` already turns the sizes into sorted sides and volumes. A
constraint that needs another table of facts builds it in `on_reset()`, which runs once when
the solve starts (and on every step under `full_assert`); call the base's first:
`PerParcelConstraint::on_reset(problem);`.

**A constraint at weight 0 costs a move nothing.** The assembly asks a unit constraint
at weight 0 only when a solve starts and when `bs.constraint_costs()` reports it: on the
version this guide was measured on, `get_constraints()` with the example returned seven
constraints, and a solve with them ran at 160 thousand steps per second where the six ran
at 163 thousand, which is within what two runs of the same build differ by.
`board_area_cost` and `shipping_cost`, added since at weight 0, are the same case. A global at
weight 0 is still sent every change, unless it declares `idle_at_weight_zero()` as
`BoxTypeCount` does. `bs.solve(problem)` is not affected.

**Keep raw costs whole numbers.** Sides, weights and quantities are integers, so sums
stay exact and a hard total is never rounded. Compute in `int64` and return
`static_cast<double>` of the result.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`bs.solve(problem)` must score it, and `bs.solve(problem)` scores with the
monolithic calculators: twice the steps per second of the assembly, and the only path
that runs parallel starts.

`bs_plugin_test` compares the default assembly with the monolithic calculators on
random assignments, so it fails from the moment the weight is not 0 until both
sides agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 10000000.0;`
(ten litres of air per loose shipment, the weight of step 2).

**2. The term, twice.** Both files include the header.

`cpp/bs/score/score_calculator.cpp` is the full recompute. It adds the soft level up box
by box, in the order the assembly does, so that a fractional `cost` gives the same
double on both paths. Keep the new total per box, and add it after the box's cost:

```cpp
    std::vector<std::int64_t> looseOfBox(boxCount, 0);
```

```cpp
        looseOfBox[box] += static_cast<std::int64_t>(looseFit.term(problem, p, box));
```

```cpp
        soft += LooseFitCost::kDefaultWeight * static_cast<double>(looseOfBox[b]);
```

with `const LooseFitCost looseFit;` before the loop over the parcels:
the component's own `term()` is the one definition of the cost.

`cpp/bs/score/incremental_calculator.cpp` keeps the same per-box totals. Declare
`std::vector<std::int64_t> looseOfBox_;`, `std::int64_t loose_ = 0;` and
`LooseFitCost looseFit_;` in `cpp/bs/score/incremental_calculator.hpp`, reset the two totals
in `rebuild()`, keep them in `insert()` (and, with `-=`, in `retract()`):

```cpp
    const auto count = static_cast<std::int64_t>(looseFit_.term(*problem_, parcelIndex, boxIndex));
    looseOfBox_[boxIndex] += count;  // soft 6: looseFitCost
    loose_ += count;
```

and add them to both branches of `score()`: after the box's cost in the loop
(`soft += LooseFitCost::kDefaultWeight * static_cast<double>(looseOfBox_[b]);`)
and, in the branch without costs, to the one sum
(`+ LooseFitCost::kDefaultWeight * static_cast<double>(loose_)`).

That is all for a sum of per-parcel terms: the default path ran at 286 thousand
steps per second with the seventh constraint in it, where the assembly of the same seven
ran at 139 thousand. A constraint over the boxes in use is kept the way
`boxTypeLimitCost` is: `insert()` and `retract()` move `boxTypes_` when a box's first
parcel joins or its last one leaves.

**3. The prices.** A constraint that is on by default should also steer the construction
and the moves, which price a size by `BoxRanking::unitPrice` (`model/pricing.hpp`). A cost
that is one number per box (as `cost` is) is a term there. This one depends on the parcel
too, so the sizes no longer have one order for all parcels. Left as it is, the search
still judges every step by the whole score: the default solve with the constraint ends on
the plan of step 2. `shippingCost` is a cost of that kind and is priced: with a weight on
it, `BoxRanking::of` groups the parcels that pay alike in every box and gives each group
its own order (`perGroup`, `orderOf`, `priceOf`); another parcel-dependent cost can join
the group key and the group's price there.

**4. What else moves.** A constraint that is on by default changes every default score.
With the edits above the parity checks pass and these fail:

- `bs_plugin_test`: the catalog row of step 3 (weight 0, off by default), and the scores
  worked out by hand in `verifyOverrides()` ("the defaults", "boxCost x 1000, no void",
  "boxWeightCost soft at weight 2", the repeated `voidVolumeCost` pick): their one parcel
  fills a sixth of its box;
- `bs_test`: "SwapBoxMove trades S for M at the limit and the score improves" (with the
  new cost the trade no longer pays), and the optima of the small instances, whose
  enumeration prices a parcel by the void and the box cost alone;
- `tests/box_sizing/test_models.py`: `test_solve_takes_the_model_and_returns_a_solved_model`
  and `test_json_round_trip_preserves_everything` (the plan and the score of the nested
  problem); `test_solve.py`: `test_the_score_on_the_result_is_a_full_recompute` (the soft
  score is no longer the void alone), `test_small_instances_are_solved_to_the_optimum` and
  `test_a_box_cost_moves_parcels_to_the_cheaper_box`;
- the tables, the score formula and the counts in [README.md](README.md),
  [python.md](python.md), `constraints.py`, `__init__.py`, `_core.pyi`, `domain.hpp`,
  `score_calculator.hpp`, `incremental_calculator.hpp` and `common.hpp`;
- the CP-SAT model the benchmark compares against,
  `benchmarks/box-sizing/cpsat/box_sizing_cpsat/model.py`: the same term in the price of a
  (group, box) pair, and a row in its tests. Without it
  [`benchmarks/box-sizing`](../../../benchmarks/box-sizing) no longer compares one
  objective. Its published results, the figures of the guide's
  [Search](README.md#search) section and `CHANGELOG.md` follow.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed. With
`build_problem()` of step 1:

```python
import time

from optimaru.box_sizing.constraints import get_constraints

steps = 1_000_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    problem = build_problem()
    config = bs.SolverConfig()
    config.stop.step_limit = steps
    config.stop.time_limit_seconds = 600
    config.run.random_seed = 1
    start = time.perf_counter()
    bs.solve(problem, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e3:6.1f} thousand steps/s  soft {problem.score.soft}")
```

Both lines end on the same soft score: same seed, same steps, same scoring. If they
differ after a change, the two paths no longer score alike.

Compare two rates only when the two solves end on the same score: then they made
the same moves, and the difference is the scoring. A step here is a change or a swap of
single parcels, or one move on the set of sizes, which reads every parcel and moves
those whose cheapest box changed: the rate depends on how many steps of each kind were
taken and on how much they moved, and a solve with other constraints takes other steps.

| Measured on the instance of step 1, a million steps | Thousand steps per second |
| --- | ---: |
| default path, six constraints | 333 |
| assembly, six constraints | 163 |
| assembly, six constraints and the constraint of step 2 at weight 0 | 160 |
| assembly, six constraints and the constraint of step 2 at ten litres | 136 |
| assembly, the same with the delta of step 4 | 124 |
| default path, seven constraints (step 5) | 286 |
| assembly, seven constraints (step 5) | 139 |

Each figure is the fastest of five runs: the machine was shared, and a run that something
else disturbed came out at as little as half the speed, while undisturbed runs of one build
were within 3% of each other. The first three rows made the same moves, and so did the last
four.
The use case had six constraints then (so the example was the seventh, in step 5 too);
`board_area_cost` and `shipping_cost` came later, at weight 0, which a move does not ask
(step 4).

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
scores at the same budget over several seeds, before and after: solve
`build_problem()` with `time_limit_seconds` and `random_seed`, with and without the
constraint in `constraints`. `./build/box_sizing/bs_demo 2` solves a small instance
(36 parcel sizes, 12 candidates) for two seconds and prints the sizes to stock.
Against CP-SAT: `uv run box-sizing-compare --seconds 10 --workers 8` in
`benchmarks/box-sizing/cpsat`
([README](../../../benchmarks/box-sizing/README.md)). The last two run the
default path only, so they measure a constraint after step 5; the CP-SAT model is
`benchmarks/box-sizing/cpsat/box_sizing_cpsat/model.py`, and a constraint that is on by
default needs its term in that model's objective, or the comparison no longer scores one
objective.

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/bs/constraints/<subject>_cost.hpp` | new | | a delta, if it pays | weight |
| `cpp/bs/constraints.hpp` | include, `emplace`, comment row | | | comment row |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | counts in the text |
| `tests/box_sizing/cpp/bs_plugin_test.cpp` | | two cases, count, catalog row | | catalog row, expected scores |
| `tests/box_sizing/test_constraint_specs.py` | | lists, info row, cases with the constraint picked | | default scores |
| `docs/use-cases/box-sizing/README.md`, `python.md`, `_core.pyi` | | table row, lists | | defaults, formula |
| `cpp/bs/score/score_calculator.cpp` | | | | the term |
| `cpp/bs/score/incremental_calculator.cpp`, `cpp/bs/score/incremental_calculator.hpp` | | | | the running totals |
| `tests/box_sizing/cpp/bs_test.cpp`, `tests/box_sizing/test_solve.py`, `test_models.py` | | | | expected scores |
| `benchmarks/box-sizing/cpsat/box_sizing_cpsat/model.py`, `benchmarks/box-sizing` | | | | the same term, results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

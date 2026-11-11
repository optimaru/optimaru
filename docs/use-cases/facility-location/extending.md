# Facility location: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its three constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | two C++ test cases, five checks that count or list the catalog |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the two monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `demandDistanceCost`, the meters a consumer stands from its facility
times the demand it draws. `distanceFromFacilityCost` counts every consumer the same; this
constraint pulls the large consumers closer than the small ones. Every snippet below was compiled
and run, and every figure measured, on optimaru 0.4.3; a later change to the solver moves the
figures, not the steps.

## How an assignment is scored

A consumer is assigned a facility. The score is kept per **unit**: one facility, with
the consumers assigned to it as its **members**. A consumer without a facility (`-1`)
is in no unit and scores nothing. A move changes one or two consumers, or all the
consumers of one facility (`CloseFacilityMove`), so only the facilities they leave
and join are scored again.

There are two scoring paths, and they give the same score for the same assignment
(`fl_plugin_test` checks it on 500 random assignments):

| | `fl.solve(problem)` | `fl.solve(problem, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/fl/constraints/` |
| Constraints | the three, at their defaults | the ones you list, with your knobs |
| Steps per second | 3.1 million | 1.5 million |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are a generated instance (600 consumers, 80 facilities), 4 million steps,
one thread of an Apple M4 Max; [step 6](#6-measure) has the script. Steps 1 to 4 work
on the assembly. Step 5 moves a constraint to the monolithic calculators.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults.
The package has no demo data, so the snippets of this guide build an instance:

```python
import random

import optimaru.facility_location as fl
from optimaru.facility_location.constraints import DistanceFromFacilityCost, FacilityCapacityCost, FacilitySetupCost


def build_problem(facility_count=80, consumer_count=600, seed=2):
    """Uniform points in the demo's box, demand 5 to 25, five times the total demand in capacity."""
    rng = random.Random(seed)

    def point():
        return fl.Location(rng.uniform(51.44, 51.56), rng.uniform(-0.16, -0.01))

    demands = [rng.randint(5, 25) for _ in range(consumer_count)]
    capacity = 5 * sum(demands) // facility_count
    facilities = [
        fl.Facility(f"F{index}", point(), max(1000, int(rng.gauss(50_000, 10_000))), capacity)
        for index in range(facility_count)
    ]
    consumers = [fl.Consumer(f"C{index}", point(), demand) for index, demand in enumerate(demands)]
    return fl.FacilityLocationProblem(facilities, consumers)


problem = build_problem()
picks = [
    FacilityCapacityCost(level="soft", weight=1000),     # hard by default
    FacilitySetupCost(weight=3),                         # heavier (default 2)
    DistanceFromFacilityCost(exponent=2, weight=0.001),  # squared per facility (default 6, linear)
]                                                        # a constraint you leave out is not scored
fl.solve(problem, time_limit_seconds=10, constraints=picks)
```

`fl.solve()` solves a `FacilityLocationProblem` in place, and a problem solved again
starts from the solution it holds: build a new one for every solve you compare.
To try weights without solving again, score a finished assignment with other picks:

```python
from optimaru.facility_location.constraints import score

raw = fl.constraint_costs(problem, picks)  # raw cost of every constraint, per facility
print(score(raw, picks))                   # the score those picks give this assignment
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/facility_location/constraints.py).
Stop here if the constraint you need is one of the three.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

[`constraints/common.hpp`](../../../optimaru/facility_location/cpp/include/fl/constraints/common.hpp)
has two bases and no shortcut on top of them:

| The constraint is | Base | You write |
| --- | --- | --- |
| a function of one facility and its consumers | `FlUnitConstraint` | `cost(problem, unit, members)` |
| a statistic across facilities (how many are open, the gap between two loads) | `FlGlobalConstraint` | `reset()`, `on_unit_changed()`, `cost()` |

The three constraints that exist are unit constraints, and so is the example: one
function. Four edits:

**1. The component**, `optimaru/facility_location/cpp/include/fl/constraints/demand_distance_cost.hpp`:

```cpp
#ifndef FL_CONSTRAINTS_DEMAND_DISTANCE_COST_HPP
#define FL_CONSTRAINTS_DEMAND_DISTANCE_COST_HPP

#include "fl/constraints/common.hpp"

namespace fl {

// Soft, off by default (weight 0): demandDistanceCost — the meters every
// assigned consumer stands from its facility, times the demand it draws. Raw
// cost per facility = sum over its consumers of demand * meters (0 for an
// unused facility), so 0 = all demand is served on the spot.
struct DemandDistanceCost final : FlUnitConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit DemandDistanceCost(double weight = kDefaultWeight)
        : FlUnitConstraint("demandDistanceCost", weight, kDefaultLevel) {}
    void on_reset(const FacilityLocationProblem& problem) override {
        detail::requirePrepared(problem);
    }
    double cost(const FacilityLocationProblem& problem, int unit,
                const std::vector<int>& members) const override {
        std::int64_t demandMeters = 0;
        for (int consumer : members) {
            demandMeters += problem.consumers[consumer].demand * problem.distanceOf(consumer, unit);
        }
        return static_cast<double>(demandMeters);
    }
};

}  // namespace fl

#endif  // FL_CONSTRAINTS_DEMAND_DISTANCE_COST_HPP
```

The name passed to the base, `"demandDistanceCost"`, is the struct name with a
lower-case first letter. Python derives `demand_distance_cost` from it.
`on_reset()` is the guard every constraint here has: `problem.distanceOf()` reads the
distance matrix that `ScoreCalculator::prepare()` builds once per problem.

The defaults live in the two constants and nowhere else; Python reads them through
`fl.constraint_info()`. `kDefaultLevel` is `Hard` or `Soft`: this use case has no
medium level, and a pick with `level="medium"` is refused. A hard constraint returns
whole numbers, because a hard contribution is rounded per facility.

**2. The assembly**, `cpp/include/fl/constraints.hpp`: include the header, add its
row to the comment that lists the set, and add one line at the end of
`defaultConstraints()`:

```cpp
    assembly.emplace<DemandDistanceCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no default solve changes and the monolithic calculators stay as they are.

**3. The Python spec**, `optimaru/facility_location/constraints.py`:

```python
class DemandDistanceCost(Constraint):
    """Distance (m) from each assigned consumer to its facility, times its demand. Opt-in: soft, weight 0."""

    name = "demand_distance_cost"
    label_en = "Demand-weighted distance"
    label_ko = "수요 가중 거리"
    description_en = (
        "For each assigned consumer: its demand times the distance in meters to its facility, summed. "
        "0 means all demand is served on the spot. Opt-in: the default weight is 0."
    )
    description_ko = (
        "배정된 고객마다 수요에 담당 시설까지의 거리(미터)를 곱해 모두 더합니다. "
        "0이면 수요가 있는 모든 고객이 담당 시설과 같은 곳에 있습니다. "
        "기본 가중치가 0이므로 가중치를 주어야 점수에 반영됩니다."
    )
```

**4. Its default instance**, in the same file: `demand_distance_cost =
DemandDistanceCost()` with the other instances, that instance at the end of
`_DEFAULTS` (the order of `defaultConstraints()`), and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=facility_location
uv sync                                    # rebuilds optimaru.facility_location._core
```

```python
from optimaru.facility_location.constraints import DemandDistanceCost, get_constraints

others = [spec for spec in get_constraints() if spec.name != "demand_distance_cost"]
picks = [*others, DemandDistanceCost(weight=1)]
config = fl.SolverConfig()
config.step_count_limit = 4_000_000
config.random_seed = 1
problem = build_problem()
fl.solve(problem, config=config, constraints=picks)
print(sum(fl.constraint_costs(problem, picks)["demand_distance_cost"]["costs"]))
```

On the instance of step 1 that sum is 11,032,950. A default solve of the same length
(`constraints=None`) leaves it at 11,801,065.

What the engine does with `cost()` alone: it keeps the raw cost of every facility,
and when a consumer leaves or joins a facility it calls `cost()` again for that
facility with its new members. That is correct for any constraint, and costs one pass
over the consumers of the facility.

## 3. Check it

**A case worked out by hand**, in `tests/facility_location/cpp/fl_plugin_test.cpp`,
with the other raw costs in `verifyRawCostsByHand()`. Every constraint there has a case
that costs and a case that costs 0; this is the first:

```cpp
    {
        // Demands 3 and 5 at the origin, both served by F1 (0.25 deg away):
        // (3 + 5) * 27750 m on F1; F0's own consumer stands on it.
        FacilityLocationProblem p = twoFacilities();
        addConsumerAt(p, 0.0, 9, 0);
        addConsumerAt(p, 0.0, 3, 1);
        addConsumerAt(p, 0.0, 5, 1);
        check(rawCosts(p, "demandDistanceCost") == std::vector<double>{0.0, 222000.0},
              "demandDistanceCost: [0, 222000] (demand 8 served from 27750 m)");
    }
```

```sh
cmake -B build -DOPTIMARU_USE_CASES=facility_location && cmake --build build -j
ctest --test-dir build --output-on-failure
```

**The counts.** A fourth constraint fails five checks that count or list the catalog.
Update them:

- `fl_plugin_test.cpp`: `constraint_totals().size() == 3` in `verifyConstraintTotals()`,
  and `rows.size() == 3` in `verifyCatalog()`. Add the new row there. `rowIs()`
  expects a constraint that is on by default; make it expect
  `row.default_enabled == (weight > 0.0)`, as vehicle routing's test does;
- `tests/facility_location/test_constraint_specs.py`:
  `test_list_constraints_matches_the_spec_names` and
  `test_module_level_instances_keep_every_default` (the lists), and
  `test_constraint_info_reports_the_cpp_defaults` (the row
  `("soft", 0.0, True, False, False)`).

**Incremental against full.** Solve with every score checked against a full
recompute. It raises `RuntimeError` ("Score corruption: incremental score ... !=
full recompute ...") if a move leaves the score stale:

```python
fl.solve(build_problem(), time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

Pick the constraint with a weight for this, as `picks` does. `fl_plugin_test` walks 3,000
random moves and solves under full checks on the default assembly, but there the
new constraint has weight 0 and adds 0 whatever its raw cost is. With a `delta_cost()`
(step 4) made wrong on purpose, `fl_plugin_test` and a `full_assert` solve of
`get_constraints()` still passed; the solve above raised. To keep the check, add a
pick list with the constraint to `test_full_assert_solves_with_repeats_and_offsets`.

**The tables.** The constraint tables in [README.md](README.md) and
[python.md](python.md) must equal `fl.constraint_info()`; add the row. The
docstrings of `list_constraints()` and `constraint_info()` in `_core.pyi` list the
constraints, and the notebook `examples/facility-location.ipynb` prints them.

## 4. Make it cheaper per move

On the assembly a move costs, for every constraint, one call for the facility a consumer
leaves and one for the facility it joins. What that call is depends on the constraint:

| The constraint is | Use | Per move | Example to copy |
| --- | --- | --- | --- |
| a function of the whole facility (a clamp, a step) | `FlUnitConstraint` with `cost()` alone | one pass over the facility's consumers | [`facility_capacity_cost.hpp`](../../../optimaru/facility_location/cpp/include/fl/constraints/facility_capacity_cost.hpp), [`facility_setup_cost.hpp`](../../../optimaru/facility_location/cpp/include/fl/constraints/facility_setup_cost.hpp) |
| a sum of one term per consumer | the same, with `supports_delta()` and `delta_cost()` | one term | [`distance_from_facility_cost.hpp`](../../../optimaru/facility_location/cpp/include/fl/constraints/distance_from_facility_cost.hpp) |
| a statistic across facilities | `optimaru::MemberChangeGlobalConstraint<FacilityLocationProblem>`: keep your own totals in `on_member_changed()` | what your update costs | none here; assembly-line balancing's [`used_stations_cost.hpp`](../../../optimaru/assembly_line_balancing/cpp/include/alb/constraints/used_stations_cost.hpp) counts the units in use |

The example is a sum of one term per consumer, so it can state what one consumer
adds:

```cpp
    // What one consumer adds to its facility, whoever else is there.
    bool supports_delta() const override { return true; }
    double delta_cost(const FacilityLocationProblem& problem, int unit,
                      const std::vector<int>& /*members*/, int entity) const override {
        return static_cast<double>(problem.consumers[entity].demand *
                                   problem.distanceOf(entity, unit));
    }
```

`delta_cost()` must equal `cost(with the consumer) - cost(without it)` exactly. The
`full_assert` solve of step 3 checks that it does, and a seeded solve ends on the
same score as before, as it must.

**Measure before you keep it.** With the constraint at weight 1, in million steps per
second:

| Instance | Consumers read per `cost()` call | `cost()` alone | with `delta_cost()` |
| --- | --- | --- | --- |
| 600 consumers, 80 facilities, 4 million steps | 18 | 1.75 | 1.64 |
| 3,000 consumers, 20 facilities, 1 million steps | 184 | 1.60 | 2.48 |

The delta is 6% slower on the first instance and 55% faster on the second. Counting
the calls shows why. On the first, 10 million `cost()` calls read 182 million
consumers, and the delta replaced them with 87 million `delta_cost()` calls: one
call saves two reads. On the second, 10.6 million `delta_cost()` calls replaced 404
million reads: one call saves 38. The delta is asked that often for two reasons
(see [`decomposed_objective.hpp`](../../../optimaru/cpp/optimaru/score/decomposed_objective.hpp)).
`CloseFacilityMove` moves a whole facility in one batch: a constraint with `cost()` alone
is asked once per touched facility when the batch ends, a delta constraint for every
consumer that moves. And when a move is rejected, the engine puts back the saved
costs of the constraints without a delta and asks nothing, while a delta constraint is asked
again for every consumer on the way back.

So keep `delta_cost()` when facilities hold hundreds of consumers, and leave it out
when they hold a few dozen. `distanceFromFacilityCost` shows the same: with its
delta switched off, the three constraints ran at 1.67 instead of 1.46 million steps per
second on the first instance. The steps below do not depend on the delta.

Three more things decide what a constraint costs.

**Compute once per solve.** The facts never change during a solve.
`ScoreCalculator::prepare()` already turns the locations into the distance matrix
that `problem.distanceOf()` reads. A constraint that needs another table of facts builds
it in `on_reset()`, which runs once when the solve starts (and on every step under
`full_assert`).

**A constraint at weight 0 is still computed.** `fl.constraint_costs()` reports it, so
the assembly keeps its raw cost on every move: `get_constraints()` now returns four
constraints, and a solve with them ran at 1.35 million steps per second where the three
ran at 1.46 million. A caller who does not want the constraint leaves it out of the list,
as `others` does in step 2. `fl.solve(problem)` is not affected.

**Keep raw costs whole numbers.** Demand, capacity, setup cost and meters are
integers, so sums stay exact and a hard total is never rounded. Add in `int64` and
return `static_cast<double>` of the sum.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`fl.solve(problem)` must score it, and `fl.solve(problem)` scores with the
monolithic calculators: twice the steps per second of the assembly, and the only
path that runs parallel starts.

`fl_plugin_test` compares the default assembly with the monolithic calculators on
random assignments, so it fails from the moment the weight is not 0 until both
sides agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 1.0;`.
It must be a whole number, because the monolithic calculators add in `int64`; a
`static_assert` in `score_calculator.cpp` refuses anything else.

**2. The term, twice.** Both files include the header and read the weight off the
component:

```cpp
constexpr auto kDemandDistanceWeight =
        static_cast<std::int64_t>(DemandDistanceCost::kDefaultWeight);
```

`cpp/src/score_calculator.cpp` is the full recompute. Add the weight to the
`static_assert`, and the sum to the loop over the consumers and to the soft level
of `ScoreCalculator::score()`:

```cpp
        demandDistance += consumer.demand * problem.distanceOf(c, consumer.facility);
```

```cpp
    score.soft = static_cast<double>(-(kSetupWeight * setupCost + kDistanceWeight * distance +
                                       kDemandDistanceWeight * demandDistance));
```

`cpp/src/incremental_calculator.cpp` keeps one running total per constraint. Declare
`std::int64_t demandDistance_ = 0;` in `cpp/include/fl/incremental_calculator.hpp`,
set it to 0 in `rebuild()`, add it to the soft level in `score()` as above, and
keep it in `insert()` (and, with `-=`, in `retract()`):

```cpp
    // soft 4: demandDistanceCost.
    demandDistance_ += consumer.demand * problem_->distanceOf(consumerIndex, facilityIndex);
```

That is all for a sum of per-consumer terms: the default path ran at 3.70 million
steps per second with the fourth constraint in it, where the assembly of the same four
ran at 1.64 million. A constraint over the whole facility is kept the way
`facilityCapacityCost` is: `insert()` and `retract()` take the facility's old term
out of the total, change `usedDemand_` and `consumerCnt_`, and put the new term in
(`capacityPenalty()`).

**3. What else moves.** A constraint that is on by default changes every default score.
With the two edits above the parity checks pass and these fail:

- `fl_test`: the hand-computed scores "distance 0.25 deg x weight 6", "distance 3-4-5
  triangle" and "combined";
- `fl_plugin_test`: "named edits == the same assembly listed by hand" (the named
  side drops one constraint from the defaults and now keeps the new one), and the catalog
  row of step 3 (weight 0, off by default);
- `test_constraint_specs.py`: `test_constraint_info_reports_the_cpp_defaults`,
  `test_weight_none_means_the_component_default` and
  `test_constraint_costs_match_capacity_setup_and_distance_by_hand`; in
  `test_solve.py`, `test_solver_score_matches_recompute`, which rebuilds the soft
  score from the two totals the problem reports;
- the tables, the score formula and the "three" in [README.md](README.md),
  [python.md](python.md), `constraints.py`, `__init__.py`, `_core.pyi`, `domain.hpp`,
  `score_calculator.hpp`, `incremental_calculator.hpp` and `common.hpp`;
- the CP-SAT model the benchmark compares against, `external_solver.facility_location`
  in [external-solver](https://github.com/optimaru/external-solver): a plugin class
  that subclasses the spec, in its `constraints.py`, and its entry in `REGISTRY`.
  Without it [`benchmarks/facility-location`](../../../benchmarks/facility-location)
  no longer compares one objective. Its published results and `CHANGELOG.md` follow.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed. With
`build_problem()` of step 1:

```python
import time

from optimaru.facility_location.constraints import get_constraints

steps = 4_000_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    problem = build_problem()
    config = fl.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    fl.solve(problem, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e6:5.2f} M steps/s  soft {problem.score.soft}")
```

Both lines end on the same soft score: same seed, same steps, same scoring. If they
differ after a change, the two paths no longer score alike.

Compare two rates only when the two solves end on the same score: then they made
the same moves, and the difference is the scoring. A step is one move or, when the
search stalls, one ruin-and-recreate kick with its polish moves (5,000 by default),
so a solve with other constraints or another step count has another rate for reasons that
are not the scoring. Below, the first three lines made the same moves, and so did
the last three.

| Measured on the instance of step 1, 4 million steps | Steps per second |
| --- | --- |
| default path, three constraints | 3.12 million |
| assembly, three constraints | 1.46 million |
| assembly, three constraints and the constraint of step 2 at weight 0 | 1.35 million |
| assembly, three constraints and the constraint of step 2 at weight 1 | 1.75 million |
| assembly, the same with the delta of step 4 | 1.64 million |
| default path, four constraints (step 5) | 3.70 million |

Each figure is the median of three runs. The same build measured an hour apart
moved by up to 6%.

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
scores at the same budget over several seeds, before and after: solve
`build_problem()` with `time_limit_seconds` and `random_seed`, with and without the
constraint in `constraints`. `./build/facility_location/fl_demo 10` solves the demo
instance (60 consumers, 30 facilities) for ten seconds and prints every new best.
Against CP-SAT: `uv run facility-location-compare --seconds 10 --workers 8` in
`benchmarks/facility-location/cpsat`
([README](../../../benchmarks/facility-location/README.md)). The last two run the
default path only, so they measure a constraint after step 5.

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/fl/constraints/<subject>_cost.hpp` | new | | `delta_cost` | weight |
| `cpp/include/fl/constraints.hpp` | include, `emplace`, comment row | | | comment row |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | counts in the text |
| `tests/facility_location/cpp/fl_plugin_test.cpp` | | two cases, count, catalog row | | builder case, catalog row |
| `tests/facility_location/test_constraint_specs.py` | | lists, info row, cases with the constraint picked | | default scores |
| `docs/use-cases/facility-location/README.md`, `python.md`, `_core.pyi` | | table row, lists | | defaults, formula |
| `cpp/src/score_calculator.cpp` | | | | the term |
| `cpp/src/incremental_calculator.cpp`, `cpp/include/fl/incremental_calculator.hpp` | | | | the running total |
| `tests/facility_location/cpp/fl_test.cpp`, `tests/facility_location/test_solve.py` | | | | expected scores |
| external-solver, `benchmarks/facility-location` | | | | the same constraint, results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

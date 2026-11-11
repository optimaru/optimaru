# Vehicle routing: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its sixteen constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | one C++ test case, two lists and a count in the tests |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header, one count in the tests |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the two monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `waitingTimeCost`, the seconds a vehicle stands at a visit before the
visit's time window opens. `vehicle_metrics` on a solution already reports that time; no constraint
prices it. Every snippet below was compiled and run, and every figure measured, on optimaru 0.4.3; a
later change to the solver moves the figures, not the steps.

## How a plan is scored

A visit is given a place on a vehicle's route. The score is kept per **unit**: one
vehicle, with the visits of its route, in order, as its **members**. Visits on no
route sit in one more list, the unassigned pool, which the per-vehicle constraints do not
evaluate. A move changes one or two routes, so only those are scored again.

There are two scoring paths. The first scores four of the sixteen constraints, so the two
agree only on a plan with no vehicle limit and no visit group (`vrp_plugin_test`
checks that on 300 random route layouts):

| | `vrp.solve(plan)` | `vrp.solve(plan, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/vrp/constraints/` |
| Constraints | the four core constraints: `vehicle_capacity_cost`, `max_end_time_cost`, `unassigned_visits_cost`, `travel_time_cost` | the ones you list, with your knobs |
| Steps per second, windows that make vehicles wait | 28.0 thousand | 13.4 thousand |
| Steps per second, every window open all day | 76.3 thousand | 13.0 thousand |
| Parallel starts | `parallel_solver_count` | `parallel_solver_count`, each start on its own copy of the picks (the C++ overload `Solver::solve(plan, calculator)`: one start) |

The speeds are a generated instance (100 visits, 10 vehicles), 100,000 steps, one
thread of an Apple M4 Max; [step 6](#6-measure) has the script. Steps 1 to 4 work on
the assembly. Step 5 moves a constraint to the monolithic calculators. Three things differ
from an assignment use case:

- `vrp.solve(plan)` does not score a vehicle limit (`max_stops`, ...), a
  `min_load_ratio` or a `fixed_cost`: pass `constraints=get_constraints()` for the
  whole set. `solve_request` does that itself when a vehicle sets one, and every
  entry point does for a plan with a [visit group](README.md#visit-groups).
- A step is not one move. A step of the iterated local search, of the descent or of
  a ruin and its repair tries hundreds of candidate routes
  ([step 4](#4-make-it-cheaper-per-move) counts them).
- With one seed and a step limit each path repeats exactly, but the two paths end on
  different plans. A visit that a ruin holds out of every list is charged nothing by
  the incremental calculator, which counts the pool list, and its penalty by
  `unassignedVisitsCost`. A finished plan scores the same on both.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults.
The examples use one generated plan: 100 visits and 10 vehicles that leave at 07:30,
half of the windows 08:00 to 12:00 and half 13:00 to 18:00.

```python
import random

import optimaru.vehicle_routing as vrp
from optimaru.vehicle_routing.constraints import MaxEndTimeCost, RouteDurationCost, get_constraints


def make_plan(visit_count=100, vehicle_count=10, seed=1):
    rng = random.Random(seed)
    locations, vehicles, visits = [], [], []
    for v in range(vehicle_count):
        locations.append(vrp.Location(rng.uniform(43.7515, 43.8093), rng.uniform(11.1772, 11.2902)))
        vehicles.append(vrp.Vehicle(f"V{v + 1}", rng.randint(20, 40), v, departure_time=7 * 3600 + 1800))
    for i in range(visit_count):
        locations.append(vrp.Location(rng.uniform(43.7515, 43.8093), rng.uniform(11.1772, 11.2902)))
        opens, closes = rng.choice(((8, 12), (13, 18)))
        service, demand = rng.choice((10, 20, 30, 40)) * 60, rng.randint(1, 2)
        visits.append(
            vrp.Visit(f"C{i + 1}", f"Visit {i + 1}", vehicle_count + i, demand, opens * 3600, closes * 3600, service)
        )
    return vrp.VehicleRoutePlan("measure", locations, vehicles, visits)


plan = make_plan()
changed = {"max_end_time_cost", "route_duration_cost", "vehicle_fixed_cost"}
picks = [
    *[rule for rule in get_constraints() if rule.name not in changed],  # the others as they are
    MaxEndTimeCost(level="soft", weight=600),                          # hard by default
    RouteDurationCost(weight=0.5),                                     # off by default (weight 0)
]                                                                      # vehicle_fixed_cost: dropped
vrp.solve(plan, time_limit_seconds=10, constraints=picks)              # solves in place
```

To try weights without solving again, score a finished plan with other picks:

```python
from optimaru.vehicle_routing.constraints import score

raw = vrp.constraint_costs(plan, picks)  # raw cost of every rule, per vehicle
print(score(raw, picks))                 # the (hard, soft) those picks give this plan, medium folded in
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/vehicle_routing/constraints.py).
Stop here if the constraint you need is one of the sixteen: three are objectives that
wait for a weight (`enclosing_circle_radius_cost`, `route_duration_cost`,
`latency_cost`), and seven cost nothing until a vehicle sets their field
(`max_travel_time`, `max_route_duration`, `max_end_time`, `max_stops`,
`max_route_radius_meters`, `min_load_ratio`, `fixed_cost`).

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

Fourteen of the sixteen constraints are a sum of one term per vehicle, and
[`constraints/common.hpp`](../../../optimaru/vehicle_routing/cpp/include/vrp/constraints/common.hpp)
has the base for that, `detail::PerVehicleConstraint`. You write one function,
`cost(plan, unit, members)`: the raw cost of vehicle `unit` driving the visits
`members` in that order. It reads the plan's facts and its arguments, and never
`Visit::vehicleIndex` or `Visit::arrivalTime`: the engine prices routes that do not
exist yet, and those two fields describe the routes that do. `common.hpp` derives a
route's times from its members instead: `detail::walkRoute(plan, unit, members,
onVisit)` calls `onVisit(visitIndex, arrival)` in route order and returns the
driving seconds, and `detail::routeMetrics(plan, unit, members)` returns the
driving seconds, the route's end time and the summed arrival delays.

A constraint that belongs to no single route (the unassigned visits, a group spread over
vehicles) is a `VrpGlobalConstraint`; [step 4](#4-make-it-cheaper-per-move) says what
that takes. The example reads arrival times, so it is one function. Four edits:

**1. The component**, `optimaru/vehicle_routing/cpp/include/vrp/constraints/waiting_time_cost.hpp`:

```cpp
#ifndef VRP_CONSTRAINTS_WAITING_TIME_COST_HPP
#define VRP_CONSTRAINTS_WAITING_TIME_COST_HPP

#include "vrp/constraints/common.hpp"

namespace vrp {

// Soft, weight 0 (opt-in, default_enabled = false): waitingTimeCost -- the
// time a vehicle stands at a visit before the visit's window opens. Raw cost
// per vehicle = the sum over its visits of max(0, minStartTime - arrival), in
// seconds; 0 = no vehicle arrives early (and the pool list). Give it a weight
// to keep vehicles from standing idle at closed doors.
struct WaitingTimeCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit WaitingTimeCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("waitingTimeCost", weight, kDefaultLevel) {}
    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u)) return 0;
        Seconds waiting = 0;
        detail::walkRoute(p, u, m, [&](int visitIndex, Seconds arrival) {
            waiting += std::max<Seconds>(0, p.visits[visitIndex].minStartTime - arrival);
        });
        return static_cast<double>(waiting);
    }
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_WAITING_TIME_COST_HPP
```

The name passed to the base, `"waitingTimeCost"`, is the struct name with a
lower-case first letter. Python derives `waiting_time_cost` from it.

The defaults live in these two constants and nowhere else; Python reads them through
`vrp.constraint_info()`. `kDefaultLevel` is `Hard`, `Medium` or `Soft`, and the weight is
in units of that level. A medium constraint states its weight in medium units
(`unassignedVisitsCost`: 1): the domain folds the medium level into the engine's score
itself, as `medium * 10^9 + soft` (`kMediumFold` in `domain.hpp`), so no weight carries
the 10^9. A hard constraint returns whole numbers, because a hard contribution is rounded per
vehicle.

**2. The assembly**, `cpp/include/vrp/constraints.hpp`: include the header, and add
one line at the end of `defaultConstraints()`:

```cpp
    assembly.emplace<WaitingTimeCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no solve changes and the monolithic calculators stay as they are.

**3. The Python spec**, `optimaru/vehicle_routing/constraints.py`:

```python
class WaitingTimeCost(Constraint):
    """Seconds vehicles stand at visits before the time window opens. Default: soft, weight 0 (opt-in)."""

    name = "waiting_time_cost"
    label_en = "Waiting time"
    label_ko = "대기 시간"
    description_en = (
        "The time each vehicle waits at its visits: for every visit it reaches before the window opens "
        "(`min_start_time`), the seconds until it opens. One unit of cost is one second of waiting; 0 means "
        "no vehicle arrives early. Opt-in: the default weight is 0."
    )
    description_ko = (
        "차량이 시간창이 열리기(`min_start_time`) 전에 도착한 방문지마다, 열릴 때까지 기다린 시간(초)을 "
        "더합니다. 대기 1초가 비용 1이며, 0이면 일찍 도착해 기다리는 차량이 없습니다. 기본 가중치가 "
        "0이므로 쓰려면 가중치를 직접 지정해야 합니다."
    )
```

**4. Its default instance**, in the same file: `waiting_time_cost = WaitingTimeCost()`
with the other instances, that instance in `_DEFAULTS` after `visit_order_cost`
(`_DEFAULTS` is in `list_constraints()` order: the per-vehicle constraints as they are
emplaced, then the two globals), and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=vehicle_routing
uv sync                                    # rebuilds optimaru.vehicle_routing._core
```

```python
from optimaru.vehicle_routing.constraints import WaitingTimeCost

others = [rule for rule in get_constraints() if rule.name != "waiting_time_cost"]
picks = [*others, WaitingTimeCost(weight=0.1)]
plan = make_plan()
config = vrp.SolverConfig()
config.step_count_limit = 100_000
config.random_seed = 1
vrp.solve(plan, config=config, constraints=picks)
raw = vrp.constraint_costs(plan, picks)
print(sum(raw["waiting_time_cost"]["costs"]), sum(raw["travel_time_cost"]["costs"]))
```

On the generated plan that prints 29,369 seconds of waiting and 5,798 of driving. A
default solve of the same length (`constraints=None`) leaves 128,786 seconds of
waiting and drives 4,823.

What the base gave you: the engine calls `cost()` for the routes a move changes and
for no others, never asks about the pool list, and reports the constraint per vehicle in
`constraint_costs()`.

## 3. Check it

**A case worked out by hand**, in `tests/vehicle_routing/cpp/vrp_plugin_test.cpp`,
next to the other cases of `verifyRawCostsByHand()`. Their plan has four visits
100 s apart on a line, 60 s of service each; vehicle 0 serves them in order and
arrives at 100, 260, 420 and 580. `byHand()` takes a positive and a zero case, each
with the raw cost of vehicle 0, vehicle 1 and the pool:

```cpp
    // Visit 1 opens at 400 and is reached at 260: 140 s. The route then runs
    // 140 s later, so visit 3 is reached at 720 and opens at 800: 80 s.
    byHand(
            "waitingTimeCost",
            [](VehicleRoutePlan& p) {
                p.visits[1].minStartTime = 400;
                p.visits[3].minStartTime = 800;
            },
            {220.0, 0.0, 0.0}, [](VehicleRoutePlan& p) { p.visits[1].minStartTime = 260; }, none);
```

```sh
cmake -B build -DOPTIMARU_USE_CASES=vehicle_routing && cmake --build build -j
ctest --test-dir build --output-on-failure     # this use case's: vrp_test, vrp_plugin_test, vrp_group_moves_test
uv run pytest -q tests/vehicle_routing tests/test_constraints_contract.py
```

**The counts.** A seventeenth constraint fails one check of `vrp_plugin_test` and three
Python tests. Update them:

- `vrp_plugin_test.cpp`: `rows.size() == 16` in `verifyCatalog()`. Add the row after
  `visitOrderCost`, `rowIs<WaitingTimeCost>(rows[14], "waitingTimeCost", Level::Soft,
  0.0, false)`; the two globals become rows 15 and 16;
- `tests/vehicle_routing/test_constraint_specs.py`: the list in
  `test_list_constraints_matches_the_spec_names`, and `EXPECTED_INFO`, which
  `test_constraint_info_reports_the_cpp_defaults` and
  `test_weight_none_means_the_component_default` read. A constraint at weight 0 also joins
  the classes of `test_the_weight_zero_objectives_are_opt_in`.

**Incremental against full.** Solve with every score checked against a full
recompute, and every preview of a move against the score after the move. It throws
`ScoreMismatchError` if they differ:

```python
vrp.solve(make_plan(), time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

`vrp_plugin_test` does the same for the default assembly, which now holds the constraint,
but at weight 0, and a constraint at weight 0 is not asked in a preview. The solve above,
with the constraint picked, is what checks its previews.

**The tables.** The constraint table in [README.md](README.md) must equal
`vrp.constraint_info()`; add the row. The constraints are also counted or listed in the
text under that table, in [cpp.md](cpp.md), in the docstrings of `constraints.py`
and `_core.pyi`, and in the comment of `common.hpp`. The notebook
`examples/vehicle-routing.ipynb` prints the constraint list.

## 4. Make it cheaper per move

On the assembly a constraint is asked in two ways. When a move is **applied**, the engine
reloads each route it changed and calls `cost()` of every constraint on it, the constraints at
weight 0 included. But most candidates are only **previewed**: the search describes
a candidate route as pieces of the routes as they are (`optimaru::ListPiece`: a run
of a route, forward or reversed, or one visit) and asks every constraint that has a weight
what the route would cost. A constraint answers from the pieces (`piecewise_cost()`), or
the engine writes the candidate route out and calls `cost()` on it.

Counted with a throwaway component on the C++ demo instance
(`buildRandomInstance(100, 10, 1)`, 30,000 steps): 8 calls of `cost()` per step at
weight 0, 762 per step at weight 1. So a constraint left at weight 0 costs next to
nothing, and what a picked constraint costs is what its preview costs:

| The constraint is | Use | Per preview | Example to copy |
| --- | --- | --- | --- |
| a function of the arrival times along the route | `detail::PerVehicleConstraint` with `cost()` on `walkRoute` or `routeMetrics` | one walk of the candidate route | step 2, [`latency_cost.hpp`](../../../optimaru/vehicle_routing/cpp/include/vrp/constraints/latency_cost.hpp) |
| a function of the route's load, service or driving totals | `detail::SummedUnitConstraint` with `cost()` and `piecewise_cost()` on the shared sums of [`route_sums.hpp`](../../../optimaru/vehicle_routing/cpp/include/vrp/constraints/route_sums.hpp) | a few array reads per piece, no route written out | [`travel_time_cost.hpp`](../../../optimaru/vehicle_routing/cpp/include/vrp/constraints/travel_time_cost.hpp), [`vehicle_capacity_cost.hpp`](../../../optimaru/vehicle_routing/cpp/include/vrp/constraints/vehicle_capacity_cost.hpp) |
| 0 on every route of some plans | the same two functions: answer 0 from a fact of the plan, decline otherwise | one test | [`visit_order_cost.hpp`](../../../optimaru/vehicle_routing/cpp/include/vrp/constraints/visit_order_cost.hpp), the example below |
| anything that spans routes | `VrpGlobalConstraint`: your own totals in `reset()` and `on_unit_changed()`, the total in `cost()`, and `supports_preview()` with `preview_cost()` | what your preview costs | [`unassigned_visits_cost.hpp`](../../../optimaru/vehicle_routing/cpp/include/vrp/constraints/unassigned_visits_cost.hpp), [`visit_group_cost.hpp`](../../../optimaru/vehicle_routing/cpp/include/vrp/constraints/visit_group_cost.hpp) |

A global without `supports_preview()` is correct and expensive: it switches the
previews off for the whole assembly. Every candidate is then applied, scored and
undone, and the iterated local search, which runs on previews only, stops running.
`piecewise_cost()` returns exactly what `cost()` returns for the written-out route,
or `false` to decline one call. `full_assert` (step 3) checks that it does.

**The example.** Waiting is not a sum along the route: it depends on when the
vehicle reaches each visit. The sums settle one case only, the plan where no window
opens after a vehicle leaves, so that no route can wait; `maxEndTimeCost` excludes
lateness the same way. Derive from `detail::SummedUnitConstraint` instead
(include `vrp/constraints/route_sums.hpp`, pass the name to that base) and add:

```cpp
    bool piecewise_cost(const VehicleRoutePlan& plan, int unit,
                        const optimaru::ListPiece* /*pieces*/, int /*count*/,
                        double& out) const override {
        if (!detail::isPool(plan, unit) && !sums_->noWaiting()) return false;
        out = 0.0;
        return true;
    }
```

`cost()` stays as it is. The assembly's steps per second, in thousands:

| Plan | Constraint at weight 0 | At 0.1, before | At 0.1, after |
| --- | --- | --- | --- |
| the generated plan | 13.1 | 7.2 | 7.2 |
| the same plan, every window open all day | 12.8 | 11.6 | 12.6 |

Before and after ended on the same score, as they must. On the plan with windows
nothing is gained: every preview declines and the route is walked, as before, and
the monolithic calculator walks such a plan too. Its distance to 13.1 is not all
the constraint's walk: with waiting priced the search finds more to try in a step. The
second row is like for like, because a constraint that is 0 everywhere changes no
decision: the constraint took 9% of the steps before and 2% after. One count in the tests
follows: `verifyPiecewiseByHand()` in `vrp_plugin_test.cpp` counts the unit
components that answer from pieces, `compared == 4`, now 5.

**Keep `on_reset()` light.** `on_reset()` runs whenever the objective is rebuilt,
and on routes that is not once per solve: a rejected ruin and repair, route
elimination or iterated local search step puts the routes back from a snapshot and
rebuilds (`SolutionSnapshot::restore` in the engine's `model/snapshot.hpp`). The
count above gave 3,078 calls of `on_reset()` in 30,000 steps, so a table over
visits and vehicles built there is paid for every ten steps. The constraints here do at
most one pass over the visits in `on_reset()` (`visit_order_cost.hpp` looks for an
order). What is kept per route goes into a shared context: `RouteSumsContext` marks
a route in `on_list_changed()` and builds its sums on the first preview that reads
them.

**Scope and whole numbers.** `scoped_units()` returns the units a constraint is ever
non-zero for, and the engine skips it on the others: `PerVehicleConstraint` returns
the vehicles, so the pool list is never asked. Return `static_cast<double>` of an
integer count: seconds, minutes and demand units keep sums exact.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). On vehicle routing
that makes it the fifth core constraint: `vrp.solve(plan)` scores with the monolithic calculators,
which run 2.1 times the steps per second of the assembly on the generated plan and 5.9 times on a
plan without waiting. `vrp_plugin_test` compares the default assembly with `ScoreCalculator::score`
on random route layouts, so it fails from the moment the weight is not 0 until both sides agree.
`vrp_test` holds the incremental calculator to `ScoreCalculator::score` and its previews to the
applied score.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 1.0;`.
It must be a whole number, because the monolithic calculators add in `int64`.

**2. The full recompute**, `cpp/src/score_calculator.cpp`. Include the header, read
the weight off the component into a `kWaitingWeight` next to `kTravelWeight`, add
weight and level to the two `static_assert`s, and add the term to
`ScoreCalculator::score()`:

```cpp
    // --- Soft: waitingTimeCost ---
    // penalize the seconds a vehicle stands at a visit before its window
    // opens (arrivalTime is the arrival, before any wait).
    for (const Visit& visit : plan.visits) {
        if (visit.vehicleIndex < 0) continue;  // unassigned: no arrival time
        if (visit.arrivalTime < visit.minStartTime) {
            score.soft -= kWaitingWeight * (visit.minStartTime - visit.arrivalTime);
        }
    }
```

**3. The incremental calculator**, `cpp/include/vrp/incremental_calculator.hpp` and
`cpp/src/incremental_calculator.cpp`. It keeps one total per constraint and the terms of
each route (`RouteTerms`), and prices a candidate route in one of two ways: by
walking it (`evaluateElements()`), on a plan where some window opens after a vehicle
leaves and whenever a candidate route may run late; or from per-route prefix sums
(`ListCache`), in O(1), on a plan where no visit waits. A new term goes into both.
For the example that is a `waiting` field in `RouteTerms`, a `waitingTotal_` kept
like `drivingTotal_` in `rebuild()`, `before_list_edit()` and `after_list_edit()`,
a fifth argument of `weightedScore()`, and one line in the walk:

```cpp
        if (arrival < visit.minStartTime) terms.waiting += visit.minStartTime - arrival;
```

The walking previews (`previewRunsByWalk()`, `previewInsertByWalk()`) add the
difference of the route's `waiting` as they do for `driving`. The prefix-sum
side (`termsFromCache()`, `previewRuns()`, `preview_insert()`) passes
`waitingTotal_` on as it is, since it runs only on a plan without waiting. A term
that is a sum along the route is more work there: its prefix sum in `ListCache`
(`rebuildCache()`), its difference per piece in `previewRuns()` and its delta in
`preview_insert()`.

With the fifth constraint the default path ran at 11.8 thousand steps per second on the
generated plan, where four constraints ran at 28.0. That is the search, not the term:
with waiting priced a step finds more to try, and the assembly with the constraint at
weight 1 fell the same way, from 13.4 to 5.9. On the plan with every window open,
where the constraint is 0 and the search is the same, it ran at 73.1 against 76.3.

**4. What else moves.** A constraint that is on by default changes every default score:

- the catalog checks of step 3 again: the row's weight in `verifyCatalog()` and in
  `EXPECTED_INFO`, and the constraint leaves `test_the_weight_zero_objectives_are_opt_in`;
- three checks that state a default score: in `vrp_test.cpp`, `testFixtures()`
  expects 1482 and 2423 soft for a vehicle that reaches a visit 2,757 s before its
  window opens (now 4239 and 5180), and in `vrp_plugin_test.cpp`,
  `verifyPartialAssemblies()` expects `travelTimeCost` alone to give the monolith's
  soft level. The Python tests pass as they are;
- the usage example of [README.md](README.md) and [python.md](python.md), which
  prints `0hard/0medium/-38soft` (now `-3600soft`);
- the constraint table, the score formula and the "four core constraints" in
  [README.md](README.md), [python.md](python.md), [cpp.md](cpp.md), `constraints.py`,
  `__init__.py`, `_core.pyi`, `bindings.cpp`, `solver.hpp`, `solver.cpp`,
  `score_calculator.hpp`, `incremental_calculator.hpp`, `common.hpp`, `CHANGELOG.md`;
- no CP-SAT model: [external-solver](https://github.com/optimaru/external-solver)
  has none for vehicle routing. The benchmark,
  [`benchmarks/vehicle-routing`](../../../benchmarks/vehicle-routing), is the
  capacitated problem against OR-Tools Routing, without time windows: this constraint is
  0 on its instances, and a constraint that is not would change what its gaps compare.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed, on the
plan of step 1:

```python
import time

steps = 100_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    plan = make_plan()  # solve() works in place: a fresh plan for every run
    config = vrp.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    vrp.solve(plan, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e3:5.1f} k steps/s  {plan.score}")
```

Each line repeats exactly from run to run, but the two lines do not end on the same
score (see [How a plan is scored](#how-a-plan-is-scored)): that the two paths score
alike is what `vrp_plugin_test` checks. A figure moves by about 2% between runs.

| Measured, in thousand steps per second | The generated plan | Every window open all day |
| --- | --- | --- |
| default path, four constraints | 28.0 | 76.3 |
| assembly, sixteen constraints | 13.4 | 13.0 |
| assembly, sixteen + the constraint of step 2 at weight 0 | 13.1 | 12.8 |
| assembly, the constraint of step 2 at weight 0.1 | 7.2 | 11.6 |
| assembly, the constraint with the preview of step 4 at weight 0.1 | 7.2 | 12.6 |
| default path, five constraints (step 5) | 11.8 | 73.1 |

In the first column a picked constraint changes the search as well as the scoring. The
second column is the scoring alone.

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
plans at the same budget over several seeds, before and after: the score, and the
raw totals the constraint trades against each other.

```python
for seed in range(1, 6):
    plan = make_plan()
    vrp.solve(plan, time_limit_seconds=10, random_seed=seed, constraints=picks)
    raw = vrp.constraint_costs(plan, picks)
    print(seed, plan.score, sum(raw["waiting_time_cost"]["costs"]), sum(raw["travel_time_cost"]["costs"]))
```

For a constraint on the default path (step 5), `./build/vehicle_routing/vrp_demo 100 10 10`
(visits, vehicles, seconds) solves the C++ demo instance on that path and prints
every new best: build the tree before and after the change into two build
directories and run the two binaries in turn. The comments on `Solver::Config` in
[`solver.hpp`](../../../optimaru/vehicle_routing/cpp/include/vrp/solver.hpp) show what
such comparisons report. Against OR-Tools Routing, the commands are in the
[README](../../../benchmarks/vehicle-routing/README.md) of `benchmarks/vehicle-routing`;
its instances run the default path and have no time windows.

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/vrp/constraints/<subject>_cost.hpp` | new | | base, `piecewise_cost` | weight |
| `cpp/include/vrp/constraints.hpp` | include, `emplace` | | | |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | the default in the text |
| `tests/vehicle_routing/cpp/vrp_plugin_test.cpp` | | case, catalog row | piecewise count | catalog row, one expected score |
| `tests/vehicle_routing/test_constraint_specs.py` | | lists | | `EXPECTED_INFO`, opt-in list |
| `docs/use-cases/vehicle-routing/README.md`, `cpp.md`, `_core.pyi`, the notebook | | table row, counts | | defaults, "four core constraints" |
| `cpp/src/score_calculator.cpp` | | | | the term |
| `cpp/include/vrp/incremental_calculator.hpp`, `cpp/src/incremental_calculator.cpp` | | | | the term, its total, the previews |
| `tests/vehicle_routing/cpp/vrp_test.cpp` | | | | expected scores |
| `benchmarks/vehicle-routing` | | | | nothing for a constraint on time windows |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

# Pickup and delivery: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its twenty-one constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | two C++ test cases, a catalog row, two lists in the tests |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the two monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `rideTimeCost`, the seconds each request spends on board, from leaving
its pickup to reaching its dropoff. `travel_time_cost` prices the driving and `latency_cost` the
time from the vehicle's departure to each stop; no constraint prices how long the goods or
passengers ride. Every snippet below was compiled and run, and every figure measured, on optimaru
0.4.3; a later change to the solver moves the figures, not the steps.

## How a plan is scored

A request is a pickup and a dropoff, and each stop is given a place on a
vehicle's route. The score is kept per **unit**: one vehicle, with the stops of
its route, in order, as its **members**. Stops on no route sit in one more list,
the unassigned pool; only `required_vehicle_cost` and `squared_order_age_cost`
evaluate it. A move changes one or two routes, so only those are scored again. A
constraint that has to see both stops of a request wherever they are (the three coupling
constraints, `unassigned_requests_cost`, `request_group_cost`) is a global constraint
and is told about every changed route.

There are two scoring paths. The first scores seven of the twenty-one constraints, so
the two agree only on a plan that sets no vehicle limit, pins nothing, binds no
request and groups none (`pdp_plugin_test` checks that on 300 random route layouts
and over 1,500 random list moves):

| | `pdp.solve(plan)` | `pdp.solve(plan, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/pdp/constraints/` |
| Constraints | the seven core constraints: `vehicle_capacity_cost`, `max_end_time_cost`, the three coupling constraints, `unassigned_requests_cost`, `travel_time_cost` | the ones you list, with your knobs |
| Steps per second | 89.2 thousand | 57.7 thousand |
| Parallel starts | `parallel_solver_count` | `parallel_solver_count`, each start on its own copy of the picks (the C++ overload `Solver::solve(plan, calculator)`: one start) |

The speeds are the generated instance `pdp.build_random_instance(100, 10, seed=7)`
(100 requests, so 200 stops, and 10 vehicles), 300,000 steps, one thread of an
Apple M4 Max; [step 6](#6-measure) has the script. Steps 1 to 4 work on the
assembly. Step 5 moves a constraint to the monolithic calculators. Three things to know:

- `pdp.solve(plan)` scores a plan with a [request group](README.md#request-groups)
  on the whole default assembly, and `solve_request` does so for a request that sets
  a vehicle limit, a load target or a fixed cost, pins a stop or binds a request. Those solves run the assembly
  path, at its speed.
- Neither path previews a move. The four coupling globals have no preview contract,
  and one such global makes the assembly apply, score and undo every candidate; the
  monolithic incremental calculator has none either. So a step is about one applied
  move: [step 4](#4-make-it-cheaper-per-move) counts what that asks of a constraint.
- With one seed and a step limit a solve repeats exactly, and the default path and
  the assembly of the default constraints make the same decisions, so they end on the same
  score (`-16066soft` on the generated instance). That makes the pair a check as well
  as a timing.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import optimaru.pickup_delivery as pdp
from optimaru.pickup_delivery.constraints import LatencyCost, MaxEndTimeCost, get_constraints

plan = pdp.build_random_instance(request_count=100, vehicle_count=10, seed=7)
changed = {"max_end_time_cost", "latency_cost", "vehicle_fixed_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    MaxEndTimeCost(level="soft", weight=600),                          # hard by default
    LatencyCost(weight=0.01),                                          # off by default (weight 0)
]                                                                      # vehicle_fixed_cost: dropped
pdp.solve(plan, time_limit_seconds=10, constraints=picks)              # solves in place
```

To try weights without solving again, score a finished plan with other picks:

```python
from optimaru.pickup_delivery.constraints import score

raw = pdp.constraint_costs(plan, picks)  # raw cost of every constraint, per vehicle
print(score(raw, picks))                 # the (hard, soft) those picks give this plan, medium folded in
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/pickup_delivery/constraints.py).
Stop here if the constraint you need is one of the twenty-one: four are objectives that
wait for a weight (`enclosing_circle_radius_cost`, `route_duration_cost`,
`latency_cost`, `squared_order_age_cost`), nine cost nothing until a vehicle or a
request sets their field (`max_travel_time`, `max_route_duration`, `max_end_time`,
`max_stops`, `max_route_radius_meters`, `min_load_ratio`, `fixed_cost`,
`pinned_stops`, `required_vehicle`), and `request_group_cost` until two requests
share a `group`.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

Fourteen of the sixteen per-route constraints derive from one base in
[`constraints/common.hpp`](../../../optimaru/pickup_delivery/cpp/include/pdp/constraints/common.hpp),
`detail::PerVehicleConstraint`. You write one function, `cost(plan, unit, members)`: the raw cost of
vehicle `unit` driving the stops `members` in that order. It reads the plan's facts and its
arguments, and never `Node::vehicleIndex` or `Node::arrivalTime`: those describe the routes as they
were last scored, not the candidate the engine asks about. `common.hpp` derives a route's times from
its members instead: `detail::walkRoute(plan, unit, members, onNode)` calls `onNode(nodeIndex,
arrival)` in route order and returns the driving seconds, and `detail::routeMetrics(plan, unit,
members)` returns the driving seconds, the route's end time and the summed latency. A node knows its
request (`Node::deliveryIndex`) and whether it is the pickup (`Node::pickup`).

Pick the base by what the constraint reads:

| The constraint reads | Base | You write |
| --- | --- | --- |
| one route, and is 0 for the pool | `detail::PerVehicleConstraint` | `cost()` |
| one route, and prices the pool too (`required_vehicle_cost`, `squared_order_age_cost`) | `PdpUnitConstraint` | `cost()`, the pool included |
| both stops of each request, wherever they sit | `RequestCouplingConstraint` | `requestCost(plan, delivery, pickupUnit, pickupPosition, dropoffUnit, dropoffPosition)` |
| anything else across routes (`request_group_cost`) | `PdpGlobalConstraint` | `reset()`, `on_unit_changed()`, `cost()` |

The example reads one route's arrival chain, so it is one function. Four edits:

**1. The component**, `optimaru/pickup_delivery/cpp/include/pdp/constraints/ride_time_cost.hpp`:

```cpp
#ifndef PDP_CONSTRAINTS_RIDE_TIME_COST_HPP
#define PDP_CONSTRAINTS_RIDE_TIME_COST_HPP

#include <utility>

#include "pdp/constraints/common.hpp"

namespace pdp {

// Soft, weight 0 (opt-in, default_enabled = false): rideTimeCost — how long
// a request stays on board. Raw cost per vehicle = the sum over the requests
// it picks up and then drops off of (arrival at the dropoff - departure from
// the pickup), in seconds; 0 = the vehicle carries nothing (and the pool
// list). A request whose dropoff is not behind its pickup on this route
// counts nothing here: the coupling rules price it. Give it a weight to keep
// goods or passengers from being driven around.
struct RideTimeCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit RideTimeCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("rideTimeCost", weight, kDefaultLevel) {}
    double cost(const PdpRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u)) return 0;
        Seconds ride = 0;
        std::vector<std::pair<int, Seconds>> onBoard;  // request, when it left its pickup
        detail::walkRoute(p, u, m, [&](int nodeIndex, Seconds arrival) {
            const Node& node = p.nodes[static_cast<std::size_t>(nodeIndex)];
            if (node.pickup) {
                onBoard.emplace_back(node.deliveryIndex,
                                     std::max(arrival, node.minStartTime) + node.serviceDuration);
                return;
            }
            for (const auto& [request, leftAt] : onBoard) {
                if (request == node.deliveryIndex) ride += arrival - leftAt;
            }
        });
        return static_cast<double>(ride);
    }
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_RIDE_TIME_COST_HPP
```

The name passed to the base, `"rideTimeCost"`, is the struct name with a lower-case
first letter. Python derives `ride_time_cost` from it. A request whose dropoff comes
first, or rides another vehicle, adds nothing: `pickupBeforeDropoffCost` and
`sameVehicleCost` price that, and a constraint priced twice would push the search twice.

The defaults live in these two constants and nowhere else; Python reads them through
`pdp.constraint_info()`. `kDefaultLevel` is `Hard`, `Medium` or `Soft`, and the weight
is in units of that level. A medium constraint states its weight in medium units
(`unassignedRequestsCost`: 1, one medium unit per minute of service left out): the
domain folds the medium level into the engine's score itself, as
`medium * 10^9 + soft` (`kMediumFold` in `domain.hpp`), so no weight carries the
10^9. A hard constraint returns whole numbers, because a hard contribution is rounded per
vehicle. The coupling constraints, `requiredVehicleCost` and `requestGroupCost` default to
`ScoreCalculator::kCouplingPenalty` (1000) and `pinnedStopsCost` to
`ScoreCalculator::kPinnedPenalty` (10^12); a new hard constraint picks its weight against
those.

**2. The assembly**, `cpp/include/pdp/constraints.hpp`: include the header, and add
one line at the end of `defaultConstraints()`:

```cpp
    assembly.emplace<RideTimeCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no solve changes and the monolithic calculators stay as they are. The
catalog lists the unit constraints first and the globals after them, so the new
constraint comes after `requiredVehicleCost` whatever line it is emplaced on.

**3. The Python spec**, `optimaru/pickup_delivery/constraints.py`:

```python
class RideTimeCost(Constraint):
    """Seconds each request spends on board, pickup to dropoff. Default: soft, weight 0 (opt-in)."""

    name = "ride_time_cost"
    label_en = "Ride time"
    label_ko = "탑승 시간"
    description_en = (
        "For every request a vehicle picks up and then drops off, the seconds from leaving the pickup to "
        "arriving at the dropoff. One unit of cost is one second on board; 0 means nothing is carried. "
        "Opt-in: the default weight is 0."
    )
    description_ko = (
        "차량이 픽업한 뒤 하차하는 요청마다, 픽업 지점을 떠난 때부터 하차 지점에 도착할 때까지의 시간(초)을 "
        "더합니다. 차량에 실려 있는 1초가 비용 1이며, 0이면 실어 나르는 요청이 없습니다. 기본 가중치가 "
        "0이므로 쓰려면 가중치를 직접 지정해야 합니다."
    )
```

**4. Its default instance**, in the same file: `ride_time_cost = RideTimeCost()`
with the other instances, that instance in `_DEFAULTS` after
`required_vehicle_cost` (`_DEFAULTS` is in `list_constraints()` order: the unit
constraints, then the five globals), and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=pickup_delivery
uv sync --reinstall-package optimaru       # rebuilds optimaru.pickup_delivery._core
```

```python
from optimaru.pickup_delivery.constraints import RideTimeCost

others = [spec for spec in get_constraints() if spec.name != "ride_time_cost"]
picks = [*others, RideTimeCost(weight=0.1)]
plan = pdp.build_random_instance(request_count=100, vehicle_count=10, seed=7)
config = pdp.SolverConfig()
config.step_count_limit = 300_000
config.random_seed = 1
pdp.solve(plan, config=config, constraints=picks)
raw = pdp.constraint_costs(plan, picks)
print(sum(raw["ride_time_cost"]["costs"]), sum(raw["travel_time_cost"]["costs"]))
```

On the generated plan that prints 90,777 seconds on board and 19,763 of driving. A
default solve of the same length (`constraints=None`) leaves 225,157 seconds on board
and drives 16,066: at weight 0.1 the search drives 23% more to cut the ride time by
60%.

What the base gave you: the engine calls `cost()` for the routes a move changes and
for no others, never asks about the pool list, and reports the constraint per vehicle in
`constraint_costs()`.

## 3. Check it

**Cases worked out by hand**, in `tests/pickup_delivery/cpp/pdp_plugin_test.cpp`,
next to the other cases of `verifyRawCostsByHand()`. Their plan
(`buildHandInstance()`) has four stops 100 s apart on a line, 60 s of service each,
windows open all day; vehicle 0 serves p1 d1 p2 d2 and arrives at 100, 260, 420 and
580. `byHand()` takes a positive and a zero case, each with the raw cost of vehicle 0,
vehicle 1 and the pool:

```cpp
    // p1 is left at 160 and d1 reached at 260, p2 left at 480 and d2 reached
    // at 580: 100 + 100 s on board.
    byHand(
            "rideTimeCost", keep, {200.0, 0.0, 0.0},
            [](PdpRoutePlan& p) { p.vehicles[0].nodeIndices.clear(); }, none);
    // d1 before p1: r1 counts nothing (pickupBeforeDropoffCost prices it).
    // r2: p2 left at 680, d2 reached at 780.
    byHand(
            "rideTimeCost", [](PdpRoutePlan& p) { p.vehicles[0].nodeIndices = {1, 0, 2, 3}; },
            {100.0, 0.0, 0.0}, [](PdpRoutePlan& p) { p.vehicles[0].nodeIndices = {1, 0}; }, none);
```

```sh
cmake -B build -DOPTIMARU_USE_CASES=pickup_delivery && cmake --build build -j
ctest --test-dir build --output-on-failure     # pdp_test, pdp_plugin_test, pdp_request_moves_test
uv run pytest -q tests/pickup_delivery tests/test_constraints_contract.py
```

**The counts.** A twenty-second constraint fails one check of `pdp_plugin_test` and three
Python tests. Update them:

- `pdp_plugin_test.cpp`: `rows.size() == 21` in `verifyCatalog()`. Add the row after
  `requiredVehicleCost`, `rowIs<RideTimeCost>(rows[16], "rideTimeCost", Level::Soft,
  0.0, false)`; the five globals become rows 17 to 21;
- `tests/pickup_delivery/test_constraint_specs.py`: the list in
  `test_list_constraints_matches_the_spec_names`, and `EXPECTED_INFO`, which
  `test_constraint_info_reports_the_cpp_defaults` and
  `test_weight_none_means_the_component_default` read.

**Incremental against full.** Solve with every score checked against a full recompute. It raises
`RuntimeError` ("Score corruption: ...", the engine's `ScoreMismatchError`) if a move leaves the
score stale:

```python
pdp.solve(plan, time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

`pdp_plugin_test` does the same for the default assembly, which now holds the constraint
at weight 0 (a constraint at weight 0 is still costed on every applied move here): 1,500
random list moves against `ScoreCalculator::score`, and a 2-second solve under
`ScoreChecks::Full`.

**The tables.** The constraint tables in [README.md](README.md) and
[python.md](python.md) must equal `pdp.constraint_info()`; add the row. The constraints are
also counted or listed in the text under the README table ("the sixteen per-vehicle
unit constraints", "the four opt-in objectives"), in the docstrings of
`constraints.py` and `_core.pyi`, and in the comments of `common.hpp` and
`constraints.hpp`. The notebook `examples/pickup-delivery.ipynb` prints the
constraint list.

## 4. Make it cheaper per move

On the assembly every candidate move is applied, scored and undone (see
[How a plan is scored](#how-a-plan-is-scored)). Applying it reloads the routes it
changed and calls `cost()` of every unit constraint on each of them, **the constraints at weight
0 included**, since `constraint_costs()` reports them; then every global is told
about each changed route. Counted with a throwaway counter in the example on the
generated plan: about five calls of `cost()` per step (4.8 at weight 0,
5.3 at weight 0.1, over 300,000 steps). So a constraint left at weight 0 is not
free here, and what a constraint costs is what its `cost()` and its updates cost:

| The constraint is | Use | Per applied move | Example to copy |
| --- | --- | --- | --- |
| a function of the arrival times along a route | `detail::PerVehicleConstraint` with `cost()` on `walkRoute` or `routeMetrics` | one walk of each changed route | step 2, [`latency_cost.hpp`](../../../optimaru/pickup_delivery/cpp/include/pdp/constraints/latency_cost.hpp) |
| a function of the stops alone (load, counts) | the same base, one pass over `members` | one pass over each changed route | [`vehicle_capacity_cost.hpp`](../../../optimaru/pickup_delivery/cpp/include/pdp/constraints/vehicle_capacity_cost.hpp), [`max_stops_cost.hpp`](../../../optimaru/pickup_delivery/cpp/include/pdp/constraints/max_stops_cost.hpp) |
| a per-request term over both stops | `RequestCouplingConstraint`: `requestCost()` | a pass over all nodes and all requests per changed route | [`paired_request_cost.hpp`](../../../optimaru/pickup_delivery/cpp/include/pdp/constraints/paired_request_cost.hpp) |
| anything across routes that you can keep up to date | `PdpGlobalConstraint`: your own totals in `reset()` and `on_unit_changed()`, the total in `cost()` | what your update costs | [`request_group_cost.hpp`](../../../optimaru/pickup_delivery/cpp/include/pdp/constraints/request_group_cost.hpp) (on `optimaru::GroupPlacement`) |

`request_group_cost.hpp` also has the preview contract (`supports_preview()`,
`preview_cost()`, and a piecewise answer of 0 for a plan with no group). It is the
only constraint here that has one, and it buys nothing until every global at a weight
above 0 has one too: one global without it switches the previews off for the whole
assembly. No unit constraint of this use case answers from pieces (`piecewise_cost()`),
for the same reason. `full_assert` (step 3) checks a preview against the applied
score whenever one is served.

**Compute what you can once per rebuild.** `on_reset()` runs when the objective is
rebuilt. A sequence-shaped objective is also rebuilt when a compound move is
rejected and the routes are put back from a snapshot (`SolutionSnapshot::restore` in
the engine's `model/snapshot.hpp`). The moves that do that (route elimination, ruin
and recreate as a move, the iterated local search) are off here by default, or need
previews: the counter saw 3 calls of `on_reset()`
in 300,000 steps. With `route_elimination_weight = 0.1` it saw 2,887 in 30,000. So
a table built in `on_reset()` is cheap with the defaults and is paid every ten steps
when such a move is switched on; keep it to a pass over the requests.

**The example.** Step 2 keeps the requests on board in a `std::vector` it allocates
in each call, and looks each dropoff's pickup up in it. One slot per request, sized
in `on_reset()` and left cleared after each call, takes both away (`mutable`, as
`cost()` is `const`; the scratch vectors of `TrolleyCapacityCost` in order picking
work the same way):

```cpp
    // One slot per request, sized once per rebuild: cost() below then needs
    // no allocation and no search for a dropoff's pickup.
    void on_reset(const PdpRoutePlan& plan) override {
        PerVehicleConstraint::on_reset(plan);
        leftAt_.assign(plan.deliveries.size(), kNotOnBoard);
    }
    double cost(const PdpRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u)) return 0;
        Seconds ride = 0;
        detail::walkRoute(p, u, m, [&](int nodeIndex, Seconds arrival) {
            const Node& node = p.nodes[static_cast<std::size_t>(nodeIndex)];
            Seconds& leftAt = leftAt_[static_cast<std::size_t>(node.deliveryIndex)];
            if (node.pickup) {
                leftAt = std::max(arrival, node.minStartTime) + node.serviceDuration;
            } else if (leftAt != kNotOnBoard) {
                ride += arrival - leftAt;
            }
        });
        for (int nodeIndex : m) {  // scratch, cleared after use
            const Node& node = p.nodes[static_cast<std::size_t>(nodeIndex)];
            leftAt_[static_cast<std::size_t>(node.deliveryIndex)] = kNotOnBoard;
        }
        return static_cast<double>(ride);
    }

private:
    static constexpr Seconds kNotOnBoard = std::numeric_limits<Seconds>::min();
    mutable std::vector<Seconds> leftAt_;  // request -> when it left its pickup
```

`cost()` returns what it returned before, and the solves end on the same scores. The
assembly's steps per second, in thousands, against 57.7 for the twenty-one constraints alone:

| | Constraint at weight 0 | At weight 0.1 |
| --- | --- | --- |
| step 2 | 50.4 | 51.4 |
| with the slots | 54.8 | 53.6 |

At weight 0 the search is the one of the twenty-one constraints, so the first column is the
constraint's own cost: 13% of a step before, 5% after. At weight 0.1 the search differs from
the first column, so compare the two rows only.

**Scope and whole numbers.** `scoped_units()` returns the units a constraint is ever
non-zero for (`PerVehicleConstraint`: the vehicles, never the pool), and the engine
skips it on the others. Return `static_cast<double>` of an integer count: seconds,
minutes and demand units keep sums exact.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). On pickup
and delivery that makes it the eighth core constraint: `pdp.solve(plan)` scores with the
monolithic calculators, which run 1.5 times the steps per second of the
assembly. `pdp_plugin_test`
compares the default assembly with `ScoreCalculator::score` on random route layouts
(`verifyAssemblyParity()`) and over random list moves (`verifyIncremental()`), and
`verifyMediumLevel()` compares the two folds of one plan ("default: the fold equals the
monolithic Score adapter"); all three fail from the moment the weight is not 0 until
both sides agree.
`pdp_test` holds the incremental calculator to `ScoreCalculator::score`.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 1.0;`.
It must be a whole number, because the monolithic calculators add in `int64`.

**2. The full recompute**, `cpp/src/score_calculator.cpp`. Include the header, read
the weight off the component into a `kRideWeight` next to `kTravelWeight`, add
weight and level to the two `static_assert`s, and add the term to
`ScoreCalculator::score()`, which has the node derived fields and each node's
position at hand:

```cpp
    // --- Soft: rideTimeCost, seconds on board ---
    // From the pickup's departure to the dropoff's arrival, for every request
    // whose two stops ride one vehicle, pickup first.
    for (const Delivery& delivery : plan.deliveries) {
        const Node& pickup = plan.nodes[static_cast<std::size_t>(delivery.pickupNodeIndex)];
        const Node& dropoff = plan.nodes[static_cast<std::size_t>(delivery.dropoffNodeIndex)];
        if (pickup.vehicleIndex < 0 || pickup.vehicleIndex != dropoff.vehicleIndex) continue;
        if (position[static_cast<std::size_t>(delivery.pickupNodeIndex)] >
            position[static_cast<std::size_t>(delivery.dropoffNodeIndex)])
            continue;
        const Seconds leftAt =
                std::max(pickup.arrivalTime, pickup.minStartTime) + pickup.serviceDuration;
        score.soft -= kRideWeight * (dropoff.arrivalTime - leftAt);
    }
```

**3. The incremental calculator**, `cpp/include/pdp/incremental_calculator.hpp` and
`cpp/src/incremental_calculator.cpp`. It keeps the per-route terms of each vehicle
(`RouteTerms`), re-derived in one walk of a changed route (`evaluateRoute()`), and
the coupling terms per request. Ride time is a per-route term: a `ride` field in
`RouteTerms`, a `rideTotal_` kept like `drivingTotal_` in `rebuild()`,
`before_list_edit()` and `after_list_edit()`, the soft level of `current_score()`,
and in the walk, with an `int position = 0` counted along it:

```cpp
        // rideTimeCost: + the dropoff's arrival, - the pickup's departure, for
        // a request whose two stops are both here, pickup first. The node
        // index of this list is current (after_list_edit sets it first).
        const auto partner = static_cast<std::size_t>(node.pickup ? delivery.dropoffNodeIndex
                                                                  : delivery.pickupNodeIndex);
        const bool carried =
                nodeList_[partner] == vehicleIndex &&
                (node.pickup ? nodePos_[partner] > position : nodePos_[partner] < position);

        previousDeparture = std::max(arrival, node.minStartTime) + node.serviceDuration;
        previousLocation = node.locationIndex;
        if (carried) terms.ride += node.pickup ? -previousDeparture : arrival;
        ++position;
```

The walk reads the calculator's node index (`nodeList_`, `nodePos_`) to find out
whether a stop's partner is on this route and on which side, so it needs no table of
its own: the dropoff adds its arrival and the pickup takes off its departure. The
index of the route being walked is current, because `after_list_edit()` sets it
before it calls `evaluateRoute()`. `pdp_test` held it to the full recompute over 500
random list moves and through a `FULL_ASSERT` solve.

With the eighth constraint the default path ran at 87.1 thousand steps per second, where
seven constraints ran at 89.2. That is not all the term: with ride time priced the search
goes elsewhere, and the two runs end on different plans. The assembly with the constraint
at weight 1 ran at 52.8.

**4. What else moves.** A constraint that is on by default changes every default score.
With the example at weight 1 these failed, and are expected to:

- the catalog checks of step 3 again: the row's weight and `default_enabled` in
  `verifyCatalog()`, and `EXPECTED_INFO` (`test_constraint_info_reports_the_cpp_defaults`,
  `test_weight_none_means_the_component_default`);
- checks that state a default soft score as driving alone: in `pdp_test.cpp` "an open
  route charges no leg back home" and "returnToDepot adds exactly the last-stop ->
  home leg"; in `pdp_plugin_test.cpp` "travelTimeCost alone reproduces the soft
  level" and eight checks of `verifyMediumLevel()`, whose hand plan carries r1 for
  100 s (soft -300 where they expect -200); in
  `tests/pickup_delivery/test_pdp.py`,
  `test_routes_are_open_unless_the_vehicle_returns_to_depot` (`-50soft` where it
  expects `-30soft`);
- `pdp_request_moves_test`, "the request moves reach better plans in the same number
  of steps": on five seeds of a default solve it asks the solve with the request moves
  to win more often than the one without (`request_ruin_recreate` and `bundle_moves`
  off). With ride time priced it won two and lost three;
- the usage examples, the constraint tables, the score text and the "seven core
  constraints" in [README.md](README.md), [python.md](python.md), `constraints.py`,
  `__init__.py`, `_core.pyi`, `bindings.cpp`, `solver.hpp`, `score_calculator.hpp`,
  `incremental_calculator.hpp`, `common.hpp`, `constraints.hpp`, and `CHANGELOG.md`;
- the CP-SAT model, `external_solver.pickup_delivery` in
  [external-solver](https://github.com/optimaru/external-solver): a plugin
  subclassing the spec, with its defaults from `constraint_info()`. The benchmark
  ([`benchmarks/pickup-delivery`](../../../benchmarks/pickup-delivery)) is not moved
  by a new default: it passes its own lists (`ENGINE_CONSTRAINTS`,
  `CPSAT_CONSTRAINTS` in `cpsat/pickup_delivery_cpsat/public_benchmark.py`), so a
  constraint enters it only when it is added to both lists and to the CP-SAT model.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed, on the
generated instance of step 1:

```python
import time

import optimaru.pickup_delivery as pdp
from optimaru.pickup_delivery.constraints import get_constraints

steps = 300_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    plan = pdp.build_random_instance(request_count=100, vehicle_count=10, seed=7)  # solve() works in place
    config = pdp.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    pdp.solve(plan, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e3:5.1f} k steps/s  {plan.score}")
```

Both lines end on the same score, `0hard/0medium/-16066soft`: same seed, same steps,
same scoring. If they differ after a change, the two paths no longer score alike.
Each figure below is the median of three runs of the script, each the median of three
solves. On a shared machine one solve can come out much slower than the others (75 against
89 once here); repeat a figure that looks off.

| Measured on the generated instance, in thousand steps per second | |
| --- | --- |
| default path, seven constraints | 89.2 |
| assembly, twenty-one constraints | 57.7 |
| assembly, twenty-one + the constraint of step 2 at weight 0 | 50.4 |
| assembly, the constraint of step 2 at weight 0.1 | 51.4 |
| assembly, the constraint with the slots of step 4, at weight 0 | 54.8 |
| assembly, the constraint with the slots of step 4, at weight 0.1 | 53.6 |
| default path, eight constraints (step 5) | 87.1 |

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
plans at the same budget over several seeds, before and after: the score, and the
raw totals the constraint trades against each other.

```python
for seed in range(1, 6):
    plan = pdp.build_random_instance(request_count=100, vehicle_count=10, seed=7)
    pdp.solve(plan, time_limit_seconds=10, random_seed=seed, constraints=picks)
    raw = pdp.constraint_costs(plan, picks)
    print(seed, plan.score, sum(raw["ride_time_cost"]["costs"]), sum(raw["travel_time_cost"]["costs"]))
```

For a constraint on the default path (step 5), `./build/pickup_delivery/pdp_demo 60 6 10`
(requests, vehicles, seconds) solves a generated instance on that path and prints
every new best: build the tree before and after the change into two build
directories and run the two binaries in turn. The comments on `Solver::Config` in
[`solver.hpp`](../../../optimaru/pickup_delivery/cpp/include/pdp/solver.hpp) show
what such comparisons report. Against CP-SAT and on the Li & Lim instances, the
commands are in the [README](../../../benchmarks/pickup-delivery/README.md) of
`benchmarks/pickup-delivery`; they score the lists named in step 5, not the
defaults.

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/pdp/constraints/<subject>_cost.hpp` | new | | `on_reset`, slots | weight |
| `cpp/include/pdp/constraints.hpp` | include, `emplace` | | | |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | the default in the text |
| `tests/pickup_delivery/cpp/pdp_plugin_test.cpp` | | cases, catalog row | | catalog row, expected soft scores |
| `tests/pickup_delivery/test_constraint_specs.py` | | lists | | `EXPECTED_INFO` |
| `docs/use-cases/pickup-delivery/README.md`, `python.md`, `_core.pyi`, the notebook | | table row, counts | | defaults, "seven core constraints" |
| `cpp/src/score_calculator.cpp` | | | | the term |
| `cpp/include/pdp/incremental_calculator.hpp`, `cpp/src/incremental_calculator.cpp` | | | | the term, its total |
| `tests/pickup_delivery/cpp/pdp_test.cpp`, `pdp_request_moves_test.cpp`, `test_pdp.py` | | | | expected scores |
| external-solver, `benchmarks/pickup-delivery` | | | | the same constraint, the lists |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

# Flight crew scheduling: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its six constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | three C++ test cases, five checks that count or list the catalog |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `unavailabilityBufferCost`, the seats whose flight lands on the day
before a day the crew member is unavailable, or leaves on the day after one. It keeps a free day on
both sides of time off. Every snippet below was compiled and run, and every figure measured, on
optimaru 0.4.3; a later change to the solver moves the figures, not the steps. Paths that start with
`cpp/` are under `optimaru/flight_crew_scheduling/`.

## How a schedule is scored

A seat on a flight (`FlightAssignment`) is assigned a crew member (`employee`). The
score is kept per **unit**: one employee, with the seats assigned to them as its
**members**. A seat nobody holds belongs to no unit and costs nothing. A change or a
swap moves one or two seats, so only the employees they leave and join are scored
again.

There are two scoring paths, and they give the same score for the same schedule
(`fcs_plugin_test` checks it on 500 random assignments):

| | `fc.solve(schedule)` | `fc.solve(schedule, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/fcs/constraints/` |
| Constraints | the six, at their defaults | the ones you list, with your knobs |
| Steps per second | 4.4 million | 1.6 million |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are a generated instance (112 seats, 40 crew members, 28 flights), 2 million
steps, one thread of an Apple M4 Max; [step 6](#6-measure) has the script. Steps 1 to 4
work on the assembly. Step 5 moves a constraint to the monolithic calculators.

## 1. Change a constraint that exists

No build. The package has no demo generator in Python, so this page uses its own
(seeded; the default arguments give the instance above):

```python
import random

import optimaru.flight_crew_scheduling as fc


def generate_schedule(days=7, round_trips_per_day=2, teams_per_airport=2, seed=0):
    """Round trips between two hubs and three outstations; two pilots and two attendants a flight."""
    rng = random.Random(seed)
    airports = [fc.Airport(code) for code in ("ICN", "PUS", "NRT", "HKG", "SIN")]
    employees, flights, seats = [], [], []
    for home in range(len(airports)):
        for skill in ["Pilot", "Pilot", "Flight attendant", "Flight attendant"] * teams_per_airport:
            days_off = {rng.randrange(days)} if rng.random() < 0.3 else set()
            employees.append(fc.Employee(f"E{len(employees)}", "", home, [skill], days_off))
    for day in range(days):
        for _ in range(round_trips_per_day):
            hub, outstation, hours = rng.randrange(2), rng.randrange(2, 5), rng.randrange(2, 6)
            out = day * 24 * 60 + rng.randrange(6, 12) * 60
            for origin, destination, departure in ((hub, outstation, out), (outstation, hub, out + (hours + 2) * 60)):
                flights.append(fc.Flight(f"F{len(flights)}", origin, departure, destination, departure + hours * 60))
                for skill in ("Pilot", "Pilot", "Flight attendant", "Flight attendant"):
                    seats.append(fc.FlightAssignment(f"S{len(seats)}", len(flights) - 1, len(seats), skill))
    return fc.FlightCrewSchedule(airports, employees, flights, seats)
```

Pass the constraints you want, with the knobs that differ from the defaults:

```python
from optimaru.flight_crew_scheduling.constraints import EmployeeUnavailabilityCost, RequiredSkillCost, get_constraints

schedule = generate_schedule()
changed = {"required_skill_cost", "employee_unavailability_cost", "transfer_between_two_flights_cost"}
picks = [
    *[rule for rule in get_constraints() if rule.name not in changed],  # the others as they are
    RequiredSkillCost(weight=500),                                      # heavier (default 100)
    EmployeeUnavailabilityCost(level="soft"),                           # hard by default
]                                                                       # transfer_between_two_flights_cost: dropped
result = fc.solve(schedule, time_limit_seconds=10, constraints=picks)   # solved in place and returned
```

To try weights without solving again, score a finished schedule with other picks:

```python
from optimaru.flight_crew_scheduling.constraints import score

raw = fc.constraint_costs(result, picks)  # raw cost of every rule, per crew member
print(score(raw, picks))                  # the score those picks give this schedule
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/flight_crew_scheduling/constraints.py).
Stop here if the constraint you need is one of the six.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

All six constraints are unit constraints: each is a function of one employee's seats.
[`constraints/common.hpp`](../../../optimaru/flight_crew_scheduling/cpp/include/fcs/constraints/common.hpp)
has two bases. Pick the base by what the constraint reads:

| The constraint reads | Base | You write |
| --- | --- | --- |
| one seat and its employee, yes or no | `PerAssignmentConstraint` | `violated(schedule, assignmentIndex, employeeIndex)` |
| the employee's itinerary as a whole (order, overlaps, first, last) | `ItineraryConstraint` | `select(stats)`, which returns one field of `ItineraryStats` |

`ItineraryStats` is filled by `ScoreCalculator::itineraryStats()`, which the monolithic
calculators call too. A new itinerary constraint adds its field and its loop there, or derives
from `FcsUnitConstraint` and writes its own `cost()`. The example judges one seat, so it
is one function. Four edits:

**1. The component**, `optimaru/flight_crew_scheduling/cpp/include/fcs/constraints/unavailability_buffer_cost.hpp`:

```cpp
#ifndef FCS_CONSTRAINTS_UNAVAILABILITY_BUFFER_COST_HPP
#define FCS_CONSTRAINTS_UNAVAILABILITY_BUFFER_COST_HPP

#include <set>

#include "fcs/constraints/common.hpp"

namespace fcs {

// Soft, off by default (weight 0): unavailabilityBufferCost — an assignment
// whose flight lands on the day before a day its employee is unavailable, or
// leaves on the day after one, so the time off starts or ends with a flight.
// Raw cost per employee = the number of such assignments, so 0 = every
// unavailable day has a free day on both sides. A flight that touches the
// unavailable day itself is employeeUnavailabilityCost's, not this rule's.
struct UnavailabilityBufferCost final : PerAssignmentConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit UnavailabilityBufferCost(double weight = kDefaultWeight)
        : PerAssignmentConstraint("unavailabilityBufferCost", weight, kDefaultLevel) {}
    bool violated(const FlightCrewSchedule& schedule, int assignmentIndex,
                  int employeeIndex) const override {
        const int flight = schedule.flightAssignments[assignmentIndex].flight;
        const std::set<DayIndex>& daysOff = schedule.employees[employeeIndex].unavailableDays;
        return daysOff.count(schedule.flightArrDay[flight] + 1) != 0 ||
               daysOff.count(schedule.flightDepDay[flight] - 1) != 0;
    }
};

}  // namespace fcs

#endif  // FCS_CONSTRAINTS_UNAVAILABILITY_BUFFER_COST_HPP
```

The name passed to the base, `"unavailabilityBufferCost"`, is the struct name with a
lower-case first letter. Python derives `unavailability_buffer_cost` from it.

The defaults live in these two constants and nowhere else: Python reads them through
`fc.constraint_info()`, the monolithic calculators through `cpp/src/monolithic_weights.hpp`.
`kDefaultLevel` is `Hard` or `Soft`. This use case has no medium level, and a pick with
`level="medium"` is refused with `ValueError`. A hard constraint returns whole numbers, because
a hard contribution is rounded per employee.

`flightDepDay` and `flightArrDay` are filled by `ScoreCalculator::prepare()`, which every
path that scores calls; `PerAssignmentConstraint::on_reset()` throws if it has not run.
The constraint reads `unavailableDays` itself: the table behind `employeeUnavailableOn()` covers
only the days the flights span, and the day after the last flight is outside it.

**2. The assembly**, `cpp/include/fcs/constraints.hpp`: include the header, and add
one line at the end of `defaultConstraints()`:

```cpp
    assembly.emplace<UnavailabilityBufferCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no default solve changes and the monolithic calculators stay as they are.

**3. The Python spec**, `optimaru/flight_crew_scheduling/constraints.py`:

```python
class UnavailabilityBufferCost(Constraint):
    """Assignments that land the day before, or leave the day after, a day off. Opt-in: soft, weight 0."""

    name = "unavailability_buffer_cost"
    label_en = "Flight next to an unavailable day"
    label_ko = "근무 불가일 전후 비행"
    description_en = (
        "Assignments whose flight lands on the day before a day the crew member is unavailable, or departs "
        "on the day after one. One unit of cost is one such assignment; 0 means every unavailable day has "
        "a free day on both sides."
    )
    description_ko = (
        "승무원이 근무할 수 없는 날의 전날에 도착하거나 다음 날에 출발하는 항공편 배정을 셉니다. "
        "그런 배정 하나가 비용 1이며, 0이면 근무 불가일 앞뒤로 하루씩 비행이 없습니다."
    )
```

**4. Its default instance**, in the same file: `unavailability_buffer_cost =
UnavailabilityBufferCost()` with the other instances, that instance at the end of
`_DEFAULTS` (the order of `defaultConstraints()`), and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=flight_crew_scheduling
uv sync                                    # rebuilds optimaru.flight_crew_scheduling._core
```

```python
from optimaru.flight_crew_scheduling.constraints import UnavailabilityBufferCost

others = [rule for rule in get_constraints() if rule.name != "unavailability_buffer_cost"]
picks = [*others, UnavailabilityBufferCost(weight=100)]
config = fc.SolverConfig()
config.step_count_limit = 2_000_000
config.random_seed = 1
schedule = generate_schedule()
fc.solve(schedule, config=config, constraints=picks)
print(sum(fc.constraint_costs(schedule, picks)["unavailability_buffer_cost"]["costs"]))
```

On the generated instance that sum is 0. A default solve of the same length
(`constraints=None`) leaves it at 4. Both end on `0hard/-6000soft`.

What the base gave you: `cost()` of an employee is the number of their seats for which
`violated()` is true, and `delta_cost()` is `violated()` for the one seat that moved. So
when a seat changes hands the engine calls `violated()` once for the employee it leaves
and once for the one it joins. The constraint is already incremental.

## 3. Check it

**A case worked out by hand**, in `tests/flight_crew_scheduling/cpp/fcs_plugin_test.cpp`,
next to the other single-component cases in `verifyPartialAssemblies()`:

```cpp
    {  // unavailabilityBufferCost alone: the employee is off on day 2; one
        // flight lands on day 1, one leaves on day 3, one flies on day 5.
        FlightCrewSchedule s = makeBase(2, 1);
        s.employees[0].unavailableDays = {2};
        const Minutes day = MINUTES_PER_DAY;
        addAssignment(s, addFlight(s, 0, 1 * day, 0, 1 * day + 60), 0);
        addAssignment(s, addFlight(s, 0, 3 * day, 0, 3 * day + 60), 0);
        addAssignment(s, addFlight(s, 0, 5 * day, 0, 5 * day + 60), 0);
        DecomposedCalculator calculator;
        calculator.emplace<UnavailabilityBufferCost>(1.0);
        checkScore("unavailabilityBufferCost alone", scoreWith(calculator, s), 0, -2.0);
    }
```

`verifyRawCostsByHand()` holds a positive and a zero case per constraint; add the pair there.

```sh
cmake -B build -DOPTIMARU_USE_CASES=flight_crew_scheduling -DOPTIMARU_ENGINE_TESTS=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure           # fcs_test, fcs_plugin_test
uv run pytest -q tests/flight_crew_scheduling tests/test_constraints_contract.py
```

**The counts.** A seventh constraint fails five checks that count or list the catalog.
Update them:

- `fcs_plugin_test.cpp`: `constraint_totals().size() == 6` in `verifyConstraintTotals()`,
  and `rows.size() == 6` in `verifyCatalog()`. Add the new row there. `rowIs()` expects a
  constraint that is on by default, so check the new row's fields directly
  (`!rows[6].default_enabled`);
- `tests/flight_crew_scheduling/test_constraint_specs.py`:
  `test_list_constraints_matches_the_spec_names` (the list),
  `test_module_level_instances_keep_every_default` (the instances) and
  `test_constraint_info_reports_the_cpp_defaults` (the row `("soft", 0.0, True, False, False)`).

**Incremental against full.** Solve with every score checked against a full
recompute. A move that leaves the score stale raises `RuntimeError`, "Score corruption:
incremental score ... != full recompute ..." (the engine's `ScoreMismatchError`):

```python
fc.solve(generate_schedule(), time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

While the default weight is 0, this solve is the only check of the constraint's incremental
score. `fcs_plugin_test` walks 3,000 random moves on the default assembly, but a constraint at
weight 0 adds 0 to both sides of every comparison: with `delta_cost()` made wrong on
purpose, both C++ tests still passed and the solve above failed.

**The tables.** The constraint tables in [README.md](README.md) and
[python.md](python.md) must equal `fc.constraint_info()`; add the row. The notebook
`examples/flight-crew-scheduling.ipynb` prints `fc.list_constraints()`.

## 4. Make it cheaper per move

On the assembly a move costs, for every constraint, one call for the employee a seat leaves
and one for the employee it joins. What that call is depends on the constraint:

| The constraint is | Use | Per move | Example to copy |
| --- | --- | --- | --- |
| a yes or no per seat | `PerAssignmentConstraint` | one `violated()` call | step 2 |
| a sum over pairs of one employee's seats | `supports_delta()` and `delta_cost()` next to the full `cost()` | one pass over the employee's other seats | [`flight_conflict_cost.hpp`](../../../optimaru/flight_crew_scheduling/cpp/include/fcs/constraints/flight_conflict_cost.hpp) |
| anything else about one itinerary (order, first, last) | `ItineraryConstraint` with `select()` alone | the itinerary sorted and `itineraryStats()` run again | [`transfer_between_two_flights_cost.hpp`](../../../optimaru/flight_crew_scheduling/cpp/include/fcs/constraints/transfer_between_two_flights_cost.hpp) |
| anything that spans employees | `optimaru::MemberChangeGlobalConstraint`: keep your own totals in `on_member_changed()` | what your update costs | none here; bed allocation's [`mixed_gender_room_cost.hpp`](../../../optimaru/bed_allocation/cpp/include/ba/constraints/mixed_gender_room_cost.hpp) |

A unit constraint with `cost()` alone is correct: the engine then recomputes the whole
employee on every change. `delta_cost()` returns what one seat adds given the others,
and must equal `cost(with the seat) - cost(without it)` exactly. `full_assert` (step 3)
checks that it does.

The third row is where the assembly spends its time: each of the three constraints without a
delta sorts the employee's seats and recomputes all four statistics to read one. The
same 2 million steps ran at 5.6 million steps per second with only the three constraints that
have a delta picked (skill, conflict, unavailability), at 2.9 with
`TransferBetweenTwoFlightsCost` added, at 2.1 with `FirstAssignmentNotDepartingFromHomeCost`
too, and at 1.5 with all six. (Other picks lead the search to other schedules, so read
these as sizes.) So give a constraint a delta if it is a sum over seats or over pairs.

The example is of the first kind, as cheap per move as a constraint gets here. What is left
is the `violated()` call itself.

**Compute once per solve.** The facts never change during a solve, so a term that
reads only facts can be a table. `on_reset()` runs before the first score:

```cpp
    // One table per solve: violated() below is then an array read.
    void on_reset(const FlightCrewSchedule& schedule) override {
        PerAssignmentConstraint::on_reset(schedule);
        const int employeeCount = static_cast<int>(schedule.employees.size());
        assignmentCount_ = static_cast<int>(schedule.flightAssignments.size());
        table_.assign(static_cast<std::size_t>(employeeCount) * assignmentCount_, 0);
        for (int employee = 0; employee < employeeCount; ++employee) {
            for (int assignment = 0; assignment < assignmentCount_; ++assignment) {
                table_[cell(assignment, employee)] = nextToDayOff(schedule, assignment, employee);
            }
        }
    }
    bool violated(const FlightCrewSchedule& /*schedule*/, int assignmentIndex,
                  int employeeIndex) const override {
        return table_[cell(assignmentIndex, employeeIndex)] != 0;
    }
```

`nextToDayOff()` is the test of step 2, moved unchanged to a private static function.
`cell()` is `employeeIndex * assignmentCount_ + assignmentIndex`, and `table_` is a
`std::vector<char>`.

With the constraint at weight 100 the assembly ran at 1.58 million steps per second before
and 1.54 after: no gain. The default path, which this change does not touch, read 4.39
and 4.33 in the same two runs, so the difference is noise. A crew member has few days
off, and two lookups in a set that small were never the cost. A table pays when the
term walks a list or compares strings: the skill and unavailability constraints read tables
that `ScoreCalculator::prepare()` builds for both scoring paths (`employeeSkillFlat`,
`employeeUnavailableFlat`).

`on_reset()` ran twice in a solve, at the start and for the final score, whether the
solve took 200,000 steps or 2 million. Under `full_assert` it runs at every check, so
the table is rebuilt every step there; 20,000 checked steps took 0.34 seconds. An
override must call `PerAssignmentConstraint::on_reset()`, which checks `prepare()`.

**Say which units the constraint can charge.** `scoped_units()` returns the units the constraint is
ever non-zero for, and the engine skips it on the others. None of the six constraints scopes.
This one could name the crew members who have a day off (10 of the 40 here). Tried, that
ran at 1.52 million steps per second: no gain. It also changes a score: `clip=False` with
an offset would charge only those ten, where the six constraints and their CP-SAT counterparts
charge every employee. So the example does not scope.

**Keep raw costs whole numbers.** Every constraint here counts seats or pairs, so sums stay
exact. Return `static_cast<double>` of an integer count.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`fc.solve(schedule)` must score it, and `fc.solve(schedule)` scores with the monolithic
calculators: 2.8 times the steps per second of the assembly, and the only path that
runs parallel starts.

`fcs_plugin_test` compares the default assembly with the monolithic calculators on
random assignments, so it fails from the moment the weight is not 0 until both sides
agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 100.0;`.
A soft weight may be any number; the monolithic calculators keep soft weights as
doubles. A hard weight must be whole, because they add the hard level in `int64`.

**2. One definition of the test.** The six constraints keep the definition of each raw cost in
`ScoreCalculator`, where the components and both calculators read it. Do the same:
declare `nextToUnavailableDay(schedule, assignmentIndex, employeeIndex)` in
`cpp/include/fcs/score_calculator.hpp` next to `unavailableEmployee()`, move the body of
`nextToDayOff()` to `cpp/src/score_calculator.cpp`, and let the component's table call it.

**3. The weight and the level**, in `cpp/src/monolithic_weights.hpp`: include the header,
add the level to the second `static_assert`, and read the weight off the component as
`constexpr double kUnavailabilityBufferWeight = UnavailabilityBufferCost::kDefaultWeight;`.

**4. The term, in both calculators.** `ScoreCalculator::score()` counts
`nextToUnavailableDay()` in its loop over the seats, next to the two per-seat constraints, and
adds the count times the weight to `score.soft`. `cpp/src/incremental_calculator.cpp`
keeps a counter (declared in `incremental_calculator.hpp`, set to 0 in `rebuild()`),
lowers it in `retract()`, adds it in `score()`, and raises it in `insert()`:

```cpp
    if (ScoreCalculator::nextToUnavailableDay(*schedule_, assignmentIndex, employeeIndex))
        ++nextToUnavailable_;
```

That is all for a per-seat constraint. The default path ran at 4.26 million steps per second
with the seventh constraint in it, against 4.36 without: about 2% less, which is also what two
runs of one build differ by. A constraint about the itinerary needs no counter of its own
there: once its count is a field of `ItineraryStats`, `refreshEmployee()` and the two
`score()` functions only carry one more total.

**5. What else moves.** A constraint that is on by default changes the default set and every
default score:

- in `fcs_plugin_test.cpp`, the default set listed by hand in `verifyAssemblyBuilder()`
  (`byHand.emplace<UnavailabilityBufferCost>()`) and the catalog row of step 3, now
  `rowIs<UnavailabilityBufferCost>(rows[6], "unavailabilityBufferCost", Level::Soft, 100.0)`.
  With these two and the term, `fcs_plugin_test` passes again;
- the same row in `test_constraint_info_reports_the_cpp_defaults`:
  `("soft", 100.0, True, False, True)`;
- the hand-computed scores. One case of `tests/flight_crew_scheduling/cpp/fcs_test.cpp`
  fails, "employeeUnavailabilityCost available (zero)": it flies the day after a day off
  and expected `0hard/0soft`, and now gets `0hard/-100soft`. No other C++ or Python
  expectation moved. `fcs_test` checks every default constraint by hand against the monolithic
  score, so give the new one a positive and a zero case there;
- the tables and the "six" in [README.md](README.md), [python.md](python.md),
  `constraints.py`, `__init__.py`, `common.hpp`, `incremental_calculator.hpp`, and the
  lists of constraints in `constraints.hpp`, `score_calculator.hpp` and `_core.pyi`;
- the CP-SAT model the benchmark compares against, `external_solver.flight_crew_scheduling`
  in [external-solver](https://github.com/optimaru/external-solver): one plugin per constraint,
  which subclasses the spec of step 2 and adds `build()`. Without it,
  [`benchmarks/flight-crew-scheduling`](../../../benchmarks/flight-crew-scheduling)
  no longer compares one objective. Then its published results, and `CHANGELOG.md`.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed:

```python
import time

steps = 2_000_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    schedule = generate_schedule()  # step 1
    config = fc.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    fc.solve(schedule, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e6:5.2f} M steps/s  {schedule.score}")
```

Both lines end on the same score: same seed, same steps, same scoring. If they differ
after a change, the two paths no longer score alike. A step is what `step_count_limit`
counts; the moves that polish a ruin-and-recreate kick are not counted, on either path.

| Measured on the generated instance, median of three runs | Steps per second |
| --- | --- |
| default path, six constraints | 4.36 million |
| assembly, six constraints | 1.57 million |
| assembly, six + the constraint of step 2 at weight 100 | 1.58 million |
| assembly, six + the constraint with the table of step 4 | 1.54 million |
| default path, seven constraints (step 5) | 4.26 million |
| assembly, seven constraints (step 5) | 1.57 million |

Two runs of one build differed by 2 to 3%. The gap between the two paths is real; the
differences among the assembly rows are not.

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
scores at the same budget over several seeds, before and after:

```python
for seed in range(1, 6):
    schedule = generate_schedule(days=14, round_trips_per_day=3, teams_per_airport=1)
    fc.solve(schedule, time_limit_seconds=5, random_seed=seed, constraints=picks)
    print(seed, schedule.score)
```

This use case has no quality bench binary. `./build/flight_crew_scheduling/fcs_demo 10`
solves the C++ demo instance for 10 seconds and prints every new best. Against CP-SAT:
`uv run flight-crew-scheduling-compare --seconds 10 --workers 8` in
`benchmarks/flight-crew-scheduling/cpsat`
([README](../../../benchmarks/flight-crew-scheduling/README.md)). Both run the default
path, so they show a constraint after step 5.

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/fcs/constraints/<subject>_cost.hpp` | new | | table | weight, shared test |
| `cpp/include/fcs/constraints.hpp` | include, `emplace` | | | |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | counts in the text |
| `tests/flight_crew_scheduling/cpp/fcs_plugin_test.cpp` | | cases, counts | | the default set |
| `tests/flight_crew_scheduling/test_constraint_specs.py` | | lists | | the row |
| `docs/use-cases/flight-crew-scheduling/README.md`, `python.md` | | table row | | defaults |
| `cpp/include/fcs/score_calculator.hpp`, `cpp/src/score_calculator.cpp` | | | | the test, the term |
| `cpp/src/monolithic_weights.hpp` | | | | weight, level |
| `cpp/include/fcs/incremental_calculator.hpp`, `cpp/src/incremental_calculator.cpp` | | | | the counter |
| `tests/flight_crew_scheduling/cpp/fcs_test.cpp` | | | | expected scores |
| external-solver, `benchmarks/flight-crew-scheduling` | | | | the same constraint, results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

# Flight Crew Scheduling (C++ / Python)

The `flight-crew-scheduling` example model (Apache-2.0; see the repository NOTICE)
ported onto this repository's C++ engine (`include/optimaru/`). A **BASIC variable** problem
that picks a crew member (`employee`) for every crew seat of a flight (`FlightAssignment`).

## Score mapping

The Java original uses `HardSoftScore` (two integer levels), so it maps onto the engine's
`HardSoftScore(int64 hard, double soft)` **as-is (identity)**. No level folding (no medium
level). The default weights are exactly those of the Java `ConstraintProvider`:

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost per employee (0 = no violation) |
|---|---|---|---|---|
| `required_skill_cost` | `RequiredSkillCost` | hard | 100 | assignments whose seat skill the employee lacks |
| `flight_conflict_cost` | `FlightConflictCost` | hard | 10 | pairs of the employee's assignments whose flights overlap in time |
| `transfer_between_two_flights_cost` | `TransferBetweenTwoFlightsCost` | hard | 1 | departure-consecutive pairs whose arrival airport differs from the next departure airport |
| `employee_unavailability_cost` | `EmployeeUnavailabilityCost` | hard | 10 | assignments touching an unavailable day (departure through arrival date) |
| `first_assignment_not_departing_from_home_cost` | `FirstAssignmentNotDepartingFromHomeCost` | soft | 1000 | earliest-departure assignments (ties all count) not leaving from home |
| `last_assignment_not_arriving_at_home_cost` | `LastAssignmentNotArrivingAtHomeCost` | soft | 1000 | latest-departure assignments (ties all count) not landing at home |

Every constraint is flippable (`level="hard"` / `level="soft"`), none is a reward and all six
are scored by default (`default_enabled`); `flight_crew.constraint_info()` reports exactly
this table from the C++ components (`kDefaultLevel` / `kDefaultWeight`). All six are unit
constraints over one employee's itinerary, so `exponent` shapes each employee's raw cost.

**0.2.0:** the constraints were renamed with the `_cost` suffix in every layer
(`required_skill` -> `required_skill_cost`, `flight_conflict` -> `flight_conflict_cost`,
`transfer_between_two_flights` -> `transfer_between_two_flights_cost`,
`employee_unavailability` -> `employee_unavailability_cost`,
`first_assignment_not_departing_from_home` -> `first_assignment_not_departing_from_home_cost`,
`last_assignment_not_arriving_at_home` -> `last_assignment_not_arriving_at_home_cost`; the
spec classes, module-level default instances, C++ structs/headers and registry names follow,
e.g. `RequiredSkill` -> `RequiredSkillCost` / `"requiredSkillCost"`). The old names are
unknown now (no aliases). The defaults are declared once, as each component's
`kDefaultWeight` / `kDefaultLevel`; the monolithic calculators read the same constants (the
`fcs::REQUIRED_SKILL_WEIGHT` ... `LAST_NOT_HOME_WEIGHT` constants of `domain.hpp` are gone).
Raw costs, levels, weights and the default score are unchanged.

**0.3.0:** every constraint takes two more knobs, `offset` and `clip` (keyword-only), and
may be picked several times. Each employee's raw cost `x` is shaped as
`sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which lowers the penalty of
every employee below the offset, an idle one included); `offset=0` is the 0.2.0 shaping.
Each pick is its own instance with its own knobs, and the instances add up:

```python
from flight_crew.constraints import FlightConflictCost, get_constraints

fc.solve(problem, constraints=[
    *get_constraints(),                                   # the defaults
    FlightConflictCost(weight=10, exponent=2, offset=1),  # + 10 x (conflicts beyond the first)^2
])
```

`fc.constraint_costs(schedule, picks)` keys the second pick `flight_conflict_cost#2`. The
CP-SAT reference takes integral offsets.

"Consecutive" follows the Java stream semantics: a pair (a, b) is consecutive when
dep(a) < dep(b) and no other assignment c of the same crew member satisfies
dep(a) <= dep(c) < dep(b) — with tied departure times, assignments at that time cannot
start a pair, and all ties at the later end count. first/last also count all ties.

## Build/run

```bash
cd cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/fcs_test         # per-constraint hand checks + random walk + FULL_ASSERT + pinned
./build/fcs_plugin_test  # plugin assembly == monolith, raw costs by hand, catalog, overrides
./build/fcs_demo 10   # reproduces the Java DemoDataGenerator scale, 10 s solve

cd ../python
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

The demo generates the same scale as the Java demo (6 airports, 5 days, 14 flights, ~60
seats, ~50 crew members) and reaches feasible (hard 0) within 10 s.

## Layout

- `cpp/include/fcs/domain.hpp` — domain + `SolutionDescriptor` specialization
  (including the pinned extension)
- `cpp/include/fcs/constraints/` — one component per constraint:
  `required_skill_cost.hpp`, `flight_conflict_cost.hpp`,
  `transfer_between_two_flights_cost.hpp`, `employee_unavailability_cost.hpp`,
  `first_assignment_not_departing_from_home_cost.hpp`,
  `last_assignment_not_arriving_at_home_cost.hpp` (+ `common.hpp`);
  `cpp/include/fcs/constraints.hpp` assembles the default set (`defaultConstraints()`)
- `cpp/src/score_calculator.cpp` — full recompute (the reference)
- `cpp/src/incremental_calculator.cpp` — before/after hooks, per-crew-member
  O(delta) incremental scoring
- `cpp/src/monolithic_weights.hpp` — the component defaults both monolithic
  calculators read, with compile-time checks that the hard weights are whole
  numbers and the levels match the components
- `cpp/src/solver.cpp` — engine wrapper (LA + ALNS)
- `python/` — pybind11 package `flight_crew` (with the pydantic v2 mirror;
  see `python/README.md` for usage)

## Differences from the original (caveat)

- Java `Airport.taxiTimeInMinutes` is a field no constraint or demo uses even in the
  original, so it was omitted.
- The demo data uses the same generation logic and scale as Java, but the RNG differs
  (std::mt19937_64 vs java.util.Random), so the instances are not byte-identical.
- `pinned` is an extension of this port (not in the original quickstart; the same pattern
  as the es/st ports).

## Usage

```sh
cd use-cases/flight-crew-scheduling/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
from datetime import datetime, timedelta

import flight_crew as fc
from flight_crew.models import (
    AirportModel, EmployeeModel, FlightAssignmentModel, FlightCrewScheduleModel, FlightModel,
)

out = datetime(2026, 1, 5, 8)
flights = [
    FlightModel(flight_number="OUT", departure_airport="ICN", departure_utc_date_time=out,
                arrival_airport="NRT", arrival_utc_date_time=out + timedelta(hours=2)),
    FlightModel(flight_number="BACK", departure_airport="NRT",
                departure_utc_date_time=out + timedelta(hours=5),
                arrival_airport="ICN", arrival_utc_date_time=out + timedelta(hours=7)),
]
problem = FlightCrewScheduleModel(
    airports=[AirportModel(code="ICN"), AirportModel(code="NRT")],
    employees=[EmployeeModel(id="P1", name="Pilot 1", home_airport="ICN", skills=["Pilot"])],
    flights=flights,
    flight_assignments=[
        FlightAssignmentModel(id=f"A{i}", flight=f.flight_number, index_in_flight=1, required_skill="Pilot")
        for i, f in enumerate(flights)
    ],
)
result = fc.solve(problem, time_limit_seconds=5.0, random_seed=42)   # also accepts a dict
print(result.score_hard, result.score_soft)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`flight_crew.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/flight-crew-scheduling/cpp -B build/flight-crew-scheduling && cmake --build build/flight-crew-scheduling -j
ctest --test-dir build/flight-crew-scheduling
```

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose search starts from `solution`, which is the model a solve returned or that model as a JSON dict:

```python
solved = fc.solve(problem, time_limit_seconds=5.0, random_seed=42)
seeded = fc.with_initial_solution(problem, solved.model_dump(mode="json"))
result = fc.solve(seeded, time_limit_seconds=5.0, random_seed=43)
print(result.score_hard, result.score_soft)
```

Assignments are matched by id. An assignment the solution leaves out is placed by the construction heuristic, and a complete solution is exactly where the search starts. Pinned assignments keep their employee on `problem`, whatever the solution says. Unknown ids raise `ValueError` before anything reaches the engine, which does not range-check.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/fcs/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`). This use case relies on the engine's generic moves only; it has no domain move yet. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` next
to the engine construction in `cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** Entities are placed in index order (First Fit). Set
`engineConfig.constructionHeuristic.entityDifficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/fcs/constraints/` (currently `required_skill_cost`, `flight_conflict_cost`,
`transfer_between_two_flights_cost`, `employee_unavailability_cost`,
`first_assignment_not_departing_from_home_cost`, `last_assignment_not_arriving_at_home_cost`):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint`, or the domain's `PerAssignmentConstraint` / `ItineraryConstraint`;
   positive raw cost = penalty, 0 = no violation) declaring
   `static constexpr double kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
   an `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `cpp/include/fcs/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator`, weighted by the component's
   `kDefaultWeight` — `fcs_plugin_test` checks they agree bit for bit — and a positive and a
   zero hand-calculated case to `fcs_test` / `fcs_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `python/flight_crew/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=[..., MyRuleCost(weight=5, level="hard")]`;
5. mirror it in the CP-SAT model in
   [`benchmarks/flight-crew-scheduling/cpsat`](../../benchmarks/flight-crew-scheduling/cpsat) (a
   plugin subclassing the spec; defaults come from `constraint_info()`) so the benchmark stays
   like for like.

## Benchmark

[`benchmarks/flight-crew-scheduling`](../../benchmarks/flight-crew-scheduling) compares this use case with OR-Tools CP-SAT on the same instances.

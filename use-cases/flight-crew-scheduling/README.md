# Flight Crew Scheduling (C++ / Python)

Timefold quickstart [flight-crew-scheduling](https://github.com/TimefoldAI/timefold-quickstarts)
ported onto this repository's C++ engine (`include/optimaru/`). A **BASIC variable** problem
that picks a crew member (`employee`) for every crew seat of a flight (`FlightAssignment`).

## Score mapping

The Java original uses `HardSoftScore` (two integer levels), so it maps onto the engine's
`HardSoftScore(int64 hard, double soft)` **as-is (identity)**. No level folding. The
constraint weights are also exactly those of the Java `ConstraintProvider`:

| Constraint | Level | Penalty |
|---|---|---|
| requiredSkill | hard | -100 per assignment |
| flightConflict | hard | -10 per overlapping same-crew assignment pair |
| transferBetweenTwoFlights | hard | -1 per disconnected consecutive assignment pair |
| employeeUnavailability | hard | -10 per assignment on an unavailable day |
| firstAssignmentNotDepartingFromHome | soft | -1000 |
| lastAssignmentNotArrivingAtHome | soft | -1000 |

"Consecutive" follows the Java stream semantics: a pair (a, b) is consecutive when
dep(a) < dep(b) and no other assignment c of the same crew member satisfies
dep(a) <= dep(c) < dep(b) — with tied departure times, assignments at that time cannot
start a pair, and all ties at the later end count. first/last also count all ties.

## Build/run

```bash
cd cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/fcs_test      # per-constraint hand checks + random walk + FULL_ASSERT + pinned
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
- `cpp/src/score_calculator.cpp` — full recompute (the reference)
- `cpp/src/incremental_calculator.cpp` — before/after hooks, per-crew-member
  O(delta) incremental scoring
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

<!-- usage-and-extending -->

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

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
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
`cpp/include/fcs/constraints/` (currently `employee_unavailability`, `first_assignment_not_departing_from_home`, `flight_conflict`, `last_assignment_not_arriving_at_home`, `required_skill`, `transfer_between_two_flights`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/fcs/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `fcs_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/flight_crew/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/flight-crew-scheduling/cpsat`](../../benchmarks/flight-crew-scheduling/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/flight-crew-scheduling`](../../benchmarks/flight-crew-scheduling) compares this use case with OR-Tools CP-SAT and Timefold Solver 2.6.0 on the same instances. Every solution is rescored by the Timefold quickstart's `ConstraintProvider`, so all three are judged by the same rules.

Latest run (30 s per solver, 3 instances): best score by the engine on 3, CP-SAT on 1, Timefold on 0 (ties count for each). Full table: [benchmarks/flight-crew-scheduling/README.md](../../benchmarks/flight-crew-scheduling/README.md#results).

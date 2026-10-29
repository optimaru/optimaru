# flight_crew

The Python package wrapping, with pybind11, the C++ port (`../cpp`) of the
flight-crew-scheduling example model. Solves the BASIC-variable problem of picking a crew member for every
crew seat of a flight with late-acceptance local search + ALNS.

## Install

```bash
uv venv
uv pip install -e . pytest pydantic
uv run pytest -q
```

## Usage

The high-level API is the single `solve`. It takes a pydantic model (or a dict of the same
shape) and returns the solved model:

```python
from datetime import datetime

import flight_crew as fc
from flight_crew.models import (
    AirportModel, EmployeeModel, FlightAssignmentModel,
    FlightCrewScheduleModel, FlightModel,
)

problem = FlightCrewScheduleModel(
    airports=[AirportModel(code="ICN"), AirportModel(code="NRT")],
    employees=[
        EmployeeModel(id="P1", home_airport="ICN", skills=["Pilot"]),
        EmployeeModel(id="C1", home_airport="ICN", skills=["Flight attendant"]),
    ],
    flights=[
        FlightModel(
            flight_number="OZ102",
            departure_airport="ICN",
            departure_utc_date_time=datetime(2026, 8, 10, 8, 0),
            arrival_airport="NRT",
            arrival_utc_date_time=datetime(2026, 8, 10, 10, 0),
        ),
    ],
    flight_assignments=[
        FlightAssignmentModel(id="A1", flight="OZ102", required_skill="Pilot"),
        FlightAssignmentModel(id="A2", flight="OZ102", required_skill="Flight attendant"),
    ],
)

result = fc.solve(problem, time_limit_seconds=5.0, random_seed=42)
print(result.score_hard, result.score_soft)
for a in result.flight_assignments:
    print(a.id, a.flight, a.employee)
```

- Times are UTC: a naive `datetime` is read as UTC, and a tz-aware one is converted to
  UTC. Unavailable days (`unavailable_days`) are a list of `date`.
- Seats with `pinned=True` keep their assignment (counted in the score, never moved).
- There is also a low-level path that builds engine objects (`fc.FlightCrewSchedule`,
  index references) directly and uses `fc.Solver`/`fc.calculate_score` — it needs no
  pydantic. For types/fields see `flight_crew/_core.pyi`.
- Fine-grained settings go through `models.SolveConfig` (serializable) or `fc.SolverConfig`:
  `fc.solve(problem, config=SolveConfig(time_limit_seconds=10, parallel_solver_count=4))`.

## Constraints (the Java ConstraintProvider as-is, HardSoftScore)

| Constraint | Level | Penalty |
|---|---|---|
| requiredSkill | hard | -100 per assignment without the skill |
| flightConflict | hard | -10 per time-overlapping same-crew assignment pair |
| transferBetweenTwoFlights | hard | -1 per consecutive assignment pair whose airports do not connect |
| employeeUnavailability | hard | -10 per assignment on an unavailable day |
| firstAssignmentNotDepartingFromHome | soft | -1000 |
| lastAssignmentNotArrivingAtHome | soft | -1000 |

The score is the original `HardSoftScore` as-is (no level folding).

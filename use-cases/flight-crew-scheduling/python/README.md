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

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost per employee (0 = no violation) |
|---|---|---|---|---|
| `required_skill_cost` | `RequiredSkillCost` | hard | 100 | assignments without the seat's skill |
| `flight_conflict_cost` | `FlightConflictCost` | hard | 10 | time-overlapping assignment pairs |
| `transfer_between_two_flights_cost` | `TransferBetweenTwoFlightsCost` | hard | 1 | consecutive assignment pairs whose airports do not connect |
| `employee_unavailability_cost` | `EmployeeUnavailabilityCost` | hard | 10 | assignments on an unavailable day |
| `first_assignment_not_departing_from_home_cost` | `FirstAssignmentNotDepartingFromHomeCost` | soft | 1000 | earliest assignments not departing from home |
| `last_assignment_not_arriving_at_home_cost` | `LastAssignmentNotArrivingAtHomeCost` | soft | 1000 | latest assignments not arriving at home |

The score is the original `HardSoftScore` as-is (no level folding). `fc.constraint_info()`
reports this catalog (`ConstraintInfo(name, default_level, default_weight, level_flippable,
reward, default_enabled)`) straight from the C++ components; every constraint is flippable,
none is a reward, all six are on by default.

Pick, re-weight or flip constraints with the specs in `flight_crew.constraints`; each takes
`(weight=None, hard=None, exponent=1.0)` (`None` = the C++ default):

```python
from flight_crew.constraints import (
    FlightConflictCost, RequiredSkillCost, get_constraints, last_assignment_not_arriving_at_home_cost,
)

fc.solve(problem, constraints=get_constraints())  # the defaults
fc.solve(problem, constraints=[RequiredSkillCost(weight=500), FlightConflictCost(hard=False),
                               last_assignment_not_arriving_at_home_cost])
```

`fc.constraint_costs(schedule)` returns the raw per-employee costs and
`flight_crew.constraints.score(costs, constraints)` rescores them in pure Python with the
engine's arithmetic.

**0.2.0:** every constraint name gained the `_cost` suffix (`required_skill` ->
`required_skill_cost`, `RequiredSkill` -> `RequiredSkillCost`, the default instance
`required_skill` -> `required_skill_cost`, and likewise for the other five); the old names
are refused as unknown. Defaults, raw costs and scores are unchanged.

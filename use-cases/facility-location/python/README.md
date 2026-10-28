# facility_location (Python)

The pybind11 package of the Timefold quickstart facility-location port (`../cpp`).
Picks one facility per consumer — `Consumer.facility` is the only
planning variable (facility index, -1 = unassigned).

Score (HardSoftScore):

- hard = -(sum over used facilities of `max(0, assigned demand - capacity)`)
- soft = -(2 x setup cost of used facilities + 6 x consumer-facility distance (m))
  - distance weight 6 = the `ConstraintWeightOverrides` value of the Java solution (provider
    default 5). Change it with `problem.distance_from_facility_weight`.

## Install / test

```bash
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

## Usage

```python
import facility_location as fl

facilities = [fl.Facility("F1", fl.Location(51.5, -0.1), 50_000, 150), ...]
consumers = [fl.Consumer("C1", fl.Location(51.52, -0.12), 15), ...]
problem = fl.FacilityLocationProblem(facilities, consumers)

fl.solve(problem, time_limit_seconds=5.0, random_seed=42)  # in-place solve
print(problem.score)  # 0hard/-...soft
print(problem.assignment())  # facility index per consumer
print(problem.total_cost(), problem.total_distance_meters())
```

For finer control build `fl.SolverConfig()` (time budget, multistart, ALNS knobs,
the `on_new_best` callback, `best_score_limit` early stop, ...) and call
`fl.Solver(cfg).solve(problem)` or `fl.solve(problem, config=cfg)`.

The pydantic v2 mirror (JSON serialization) is `facility_location.models`:

```python
from facility_location.models import FacilityLocationProblemModel

model = FacilityLocationProblemModel.from_core(problem)
problem2 = FacilityLocationProblemModel.model_validate_json(model.model_dump_json()).to_core()
```

Note: `problem.consumers[i]` returns a copy — write assignments in bulk with
`problem.set_assignment([...])`.

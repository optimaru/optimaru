# optimaru.facility_location (Python)

The pybind11 package of the facility-location use case (C++ in `optimaru/facility_location/cpp`).
Picks one facility per consumer — `Consumer.facility` is the only
decision variable (facility index, -1 = unassigned).

Score, at the default weights:

- hard = -(sum over used facilities of `max(0, assigned demand - capacity)`) — `facility_capacity_cost`, hard x1
- soft = -(2 x setup cost of used facilities + 6 x consumer-facility distance (m)) —
  `facility_setup_cost` soft x2, `distance_from_facility_cost` soft x6

The problem carries no weights. Pick, re-weight or flip constraints with
`optimaru.facility_location.constraints`, and read the defaults with `fl.constraint_info()`:

```python
from optimaru.facility_location.constraints import (
    DistanceFromFacilityCost, FacilityCapacityCost, FacilitySetupCost, score,
)

fl.constraint_info()
# [ConstraintInfo(name='facility_capacity_cost', default_level='hard', default_weight=1.0,
#                 level_flippable=True, reward=False, default_enabled=True), ...]
fl.solve(problem, time_limit_seconds=1.0, constraints=[
    FacilityCapacityCost(), FacilitySetupCost(), DistanceFromFacilityCost(weight=5),
])                                        # distance weighted 5 instead of 6
raw = fl.constraint_costs(problem)        # raw per-facility costs, re-priced in pure Python:
score(raw), score(raw, [DistanceFromFacilityCost(exponent=2.0)])
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping per facility (clip=True: only the distance beyond 500 m counts).
score(raw, [DistanceFromFacilityCost(), DistanceFromFacilityCost(weight=0.01, exponent=2, offset=500)])
```

## Install and test

```bash
OPTIMARU_USE_CASES=facility_location uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/facility_location
```

## Usage

```python
from optimaru import facility_location as fl

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

The pydantic v2 mirror (JSON serialization) is `optimaru.facility_location.models`:

```python
from optimaru.facility_location.models import FacilityLocationProblemModel

model = FacilityLocationProblemModel.from_core(problem)
problem2 = FacilityLocationProblemModel.model_validate_json(model.model_dump_json()).to_core()
```

Note: `problem.consumers[i]` returns a copy — write assignments in bulk with
`problem.set_assignment([...])`.

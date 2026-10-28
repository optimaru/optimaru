# Facility location CP-SAT

OR-Tools CP-SAT alternative for the canonical `facility_location.models`
contract.  It assigns every consumer, minimizes capacity overflow first, then
minimizes the engine's weighted setup and distance cost.  The final assignment
is re-scored by the shared C++ evaluator before it is returned.

```python
from facility_location_cpsat import solve

solution = solve(problem, time_limit_seconds=10, random_seed=0)
```

## Engine comparison

The comparison runner is owned by this use case. It builds deterministic
instances, gives both backends the same solve budget, and re-scores both answers
with the engine evaluator.

```sh
uv run facility-location-compare --seconds 10
uv run facility-location-compare --instance demo --seconds 2 --json
```

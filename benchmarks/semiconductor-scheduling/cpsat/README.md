# Semiconductor fab scheduling CP-SAT

OR-Tools CP-SAT reference for the canonical `semiconductor_scheduling.models` contract.
An interval model: per operation a start and an end, and per eligible machine an optional
interval of that option's duration (for a machine listed twice, the LAST option — the one
the engine scores); exactly one option is chosen. The returned schedule (batch ids per
machine, family and start) is re-scored by the C++ engine with the same selection.

```python
from semiconductor_scheduling_cpsat import solve

solution = solve(problem, time_limit_seconds=10, random_seed=0, num_workers=8)
```

## Constraints

Every constraint is a `cpsat_plugins.ConstraintPlugin` subclass of the domain spec of the
same name (`semiconductor_scheduling_cpsat/constraints.py`); default levels and weights
come from `semiconductor_scheduling.constraint_info()`, and the catalog equals it.

- **Structural (12)**: `machine_eligibility_cost` (the variable layout),
  `lot_route_precedence_cost`, `machine_capacity_cost` (no-overlap),
  `batch_capacity_cost` (cumulative), `batch_family_conflict_cost`,
  `missing_setup_time_cost`, `machine_unavailability_cost`, `min_queue_time_cost`,
  `missing_transport_time_cost`, `auxiliary_resource_capacity_cost` (cumulative, one
  demand per listing), `auxiliary_resource_unavailability_cost`, `pinned_assignments_cost`.
  A selected structural constraint is posted as model constraints, so its raw cost is 0 on
  every CP-SAT solution; `hard=False` or a weight other than 1 is refused (ValueError). An
  unselected one is not enforced.
- **Penalized (8)**: `max_queue_time_cost` and `max_cycle_time_cost` (hard by default),
  `machine_preference_cost`, `min_batch_size_cost` (groups of equal start and family),
  `machine_load_imbalance_cost`, `weighted_tardiness_cost`, `cycle_time_cost`,
  `makespan_cost`: each adds its exact raw cost on the level the selection asks for.

Stricter than the engine (never laxer): `missing_setup_time_cost` is posted for every pair
of runs on a machine, not only consecutive ones, so a setup matrix that breaks the triangle
inequality may exclude schedules the engine scores 0. Not supported on CP-SAT (refused):
`exponent != 1` and non-integer weights.

**0.3.0:** a constraint may be picked several times (as specs or as
`{"name", "weight", "hard", "offset", "clip"}` mappings); each pick is built on its own
with its own knobs (a repeated structural pick posts its rule again and is checked with its
own weight and level). Every engine constraint is global, so a penalized plugin adds its
total as one term and an integral `offset` shapes it exactly as the engine does in both
`clip` modes; a fractional offset is refused (`offset must be an integer for CP-SAT`). A
structural plugin has no term to shape: an offset on it is refused (`offset is not
supported by this CP-SAT model`), like a weight or a level.

## Tests

`tests/test_parity.py`: on fixtures with every operation fixed (`_fixed_assignments`) and
one constraint selected, a penalized plugin's objective term (`_proxy_score`) equals the
engine's raw cost and a structural plugin accepts the schedule exactly when the engine's
raw cost is 0 — a positive and a zero fixture per constraint (cycle time and makespan are
positive whenever every operation is assigned). Plus the catalog parity, a demotion scored
like the engine, the structural refusals and a C++-rescored optimum with hard 0; and
selections with repeated and shifted picks scored exactly like the engine on every fixture
(the objective per level == `score(constraint_costs(core, picks), picks)`).

## Engine comparison

```sh
uv run semiconductor-scheduling-compare --seconds 10 --workers 8   # --markdown, --json
```

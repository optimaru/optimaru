# Pickup and delivery CP-SAT

Pickup/dropoff-coupled optional circuits with same-vehicle assignment,
pickup-first precedence, running capacity, time windows, optional requests,
and travel minimization. Routes are open by default; a vehicle with
`return_to_depot=True` also drives (and is timed) back to its home. The package
shares `optimaru.pickup_delivery.models` with the C++ backend.

## Constraints

Each plugin in [`external_solver.pickup_delivery.constraints`](https://github.com/optimaru/external-solver/blob/v0.2.2/src/external_solver/pickup_delivery/constraints.py) subclasses the engine spec of
the same name (`optimaru.pickup_delivery.constraints`) and only builds its CP-SAT expression; the
default level and weight come from `optimaru.pickup_delivery.constraint_info()`, so the two
solvers cannot disagree on a default. Selections use the engine's names and knobs
(`weight=None` = default, `level=None` = default level, `level="hard"`, `"medium"` or
`"soft"` moves a pick onto that level, as in the engine). `exponent != 1` and non-integer
weights are rejected (a CP-SAT-only limitation). The objective is lexicographic over
`("hard", "medium", "soft")`; `unassigned_requests_cost` is medium at weight 1, the
engine's default in medium units.

- `paired_request_cost`, `same_vehicle_cost`, `pickup_before_dropoff_cost` are
  structural here: the circuit model assigns a request to one vehicle or none and
  orders its pickup first, so their raw cost is always 0 and they add no objective
  term. Their plugins refuse any level but hard (`level="medium"` or `"soft"`) and any
  weight other than the engine default (1000); leaving them out of a selection does not
  lift the structure.
- `enclosing_circle_radius_cost`, `squared_order_age_cost`, `pinned_stops_cost` and
  `required_vehicle_cost` have no CP-SAT plugin (engine-only); naming them here is an
  unknown-constraint error.

**Repeats, `offset` and `clip`.** A constraint may be picked several times (as
specs or as `{"name", "weight", "level", "offset", "clip"}` mappings): each pick builds its
own terms with its own knobs, keyed `name`, `name#2`, … like the engine's
`constraint_costs()`. Every plugin adds one term per engine unit — per vehicle, idle ones
included (a constant 0 where the vehicle's limit is unset), never the unassigned pool,
which the engine's route constraints do not evaluate — and the globals one term with
their total (a constant 0 for the three structural rules), so an integral `offset` shapes
each unit exactly as the engine does, in both `clip` modes; a fractional offset is refused
(`offset must be an integer for CP-SAT`). `max_route_radius_cost`, `vehicle_fixed_cost`
and `min_capacity_utilization_cost` work on whole numbers here (meters and the load target
rounded up, the fixed cost rounded), with or without an offset. `clip=False` with an offset
is refused on hard with the engine's message (`constraint 'vehicle_capacity_cost':
clip=False with a non-zero offset cannot be hard, ...`), the three structural rules
included, since they stay hard; medium folds into soft, as in the engine, so it takes the
literal formula.

`catalog()` lists the plugins in `list_constraints()` order;
external-solver's `tests/pickup_delivery/test_solve.py::test_cpsat_defaults_are_the_engine_defaults` keeps it equal to
the engine's `constraint_info()` rows.

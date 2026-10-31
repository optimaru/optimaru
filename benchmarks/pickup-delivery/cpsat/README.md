# Pickup and delivery CP-SAT

Pickup/dropoff-coupled optional circuits with same-vehicle assignment,
pickup-first precedence, running capacity, time windows, optional requests,
and travel minimization. Routes are open by default; a vehicle with
`return_to_depot=True` also drives (and is timed) back to its home. The package
shares `pdp_routing.models` with the C++ backend.

## Constraints (0.2.0)

Each plugin in `pickup_delivery_cpsat/constraints.py` subclasses the engine spec of
the same name (`pdp_routing.constraints`) and only builds its CP-SAT expression; the
default level and weight come from `pdp_routing.constraint_info()`, so the two
solvers cannot disagree on a default. Selections use the engine's names and knobs
(`weight=None` = default, `hard=None` = default level, `hard=False` = soft — from a
hard or the medium level, as in the engine). `exponent != 1` and non-integer
weights are rejected (a CP-SAT-only limitation). The objective is lexicographic over
`("hard", "medium", "soft")`; `unassigned_requests_cost` is medium at weight 1, the
engine's default in medium units.

- `paired_request_cost`, `same_vehicle_cost`, `pickup_before_dropoff_cost` are
  structural here: the circuit model assigns a request to one vehicle or none and
  orders its pickup first, so their raw cost is always 0 and they add no objective
  term. Their plugins refuse `hard=False` and any weight other than the engine
  default (1000); leaving them out of a selection does not lift the structure.
- `enclosing_circle_radius_cost`, `squared_order_age_cost`, `pinned_stops_cost` and
  `required_vehicle_cost` have no CP-SAT plugin (engine-only); naming them here is an
  unknown-constraint error.

`catalog()` lists the plugins in `list_constraints()` order;
`tests/test_solve.py::test_cpsat_defaults_are_the_engine_defaults` keeps it equal to
the engine's `constraint_info()` rows.

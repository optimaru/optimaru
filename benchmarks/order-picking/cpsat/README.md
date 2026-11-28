# Order picking CP-SAT

CP-SAT alternative with independently selectable bucket, travel and order
consolidation plugins.

The plugins ([`external_solver.order_picking.constraints`](https://github.com/optimaru/external-solver/blob/v0.3.0/src/external_solver/order_picking/constraints.py)) subclass the domain specs of
the same name (`TrolleyCapacityCost`, `DistanceFromPreviousPickTaskCost`,
`DistanceFromLastPickTaskToPathOriginCost`, `OrderSplitByTrolleyCost`, in
`list_constraints()` order). Their default level and weight come from the
engine's `order_picking.constraint_info()`, so the catalog cannot drift from the
C++ components (external-solver's `tests/order_picking/test_solve.py` checks it); `level="soft"` / `level="hard"`
moves a term to the soft / hard level on both solvers, and `level="medium"` is refused
on both (`this use case has no medium level`). Like every CP-SAT reference,
`exponent != 1` and non-integer weights are rejected.

The model measures in the default warehouse (5 columns x 3 shelving rows of shelvings 2 m x
10 m, 3 m apart). The engine also takes a grid of another size and named locations with a
distance table (`warehouse`, `locations` and `distances` on the request); the pinned
external-solver does not read those fields, so the comparison stays on the default layout.

A constraint may be picked several times (as specs or as
`{"name", "weight", "level", "offset", "clip"}` mappings); each pick builds its own
terms with its own knobs, its level included. Every plugin adds one term per trolley,
an empty one included, so an integral `offset` shapes each trolley exactly as the
engine does in both `clip` modes; a fractional offset is refused (`offset must be an
integer for CP-SAT`). `clip=False` with an offset is refused on hard with the engine's
message (`constraint 'trolley_capacity_cost': clip=False with a non-zero offset cannot be
hard, ...`).

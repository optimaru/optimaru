# order-picking-cpsat

CP-SAT alternative with independently selectable bucket, travel and order
consolidation plugins.

The plugins (`order_picking_cpsat/constraints.py`) subclass the domain specs of
the same name (`TrolleyCapacityCost`, `DistanceFromPreviousPickTaskCost`,
`DistanceFromLastPickTaskToPathOriginCost`, `OrderSplitByTrolleyCost`, in
`list_constraints()` order). Their default level and weight come from the
engine's `order_picking.constraint_info()`, so the catalog cannot drift from the
C++ components (`tests/test_solve.py` checks it); `level="soft"` / `level="hard"`
moves a term to the soft / hard level on both solvers. Like every CP-SAT
reference, `exponent != 1` and non-integer weights are rejected.

**0.3.0:** a constraint may be picked several times (as specs or as
`{"name", "weight", "hard", "offset", "clip"}` mappings); each pick builds its own
terms with its own knobs. Every plugin adds one term per trolley, an empty one
included, so an integral `offset` shapes each trolley exactly as the engine does in
both `clip` modes; a fractional offset is refused (`offset must be an integer for
CP-SAT`).

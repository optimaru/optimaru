# order-picking-cpsat

CP-SAT alternative with independently selectable bucket, travel and order
consolidation plugins.

The plugins (`order_picking_cpsat/constraints.py`) subclass the domain specs of
the same name (`TrolleyCapacityCost`, `DistanceFromPreviousPickTaskCost`,
`DistanceFromLastPickTaskToPathOriginCost`, `OrderSplitByTrolleyCost`, in
`list_constraints()` order). Their default level and weight come from the
engine's `order_picking.constraint_info()`, so the catalog cannot drift from the
C++ components (`tests/test_solve.py` checks it); `hard=False` / `hard=True`
moves a term to the soft / hard level on both solvers. Like every CP-SAT
reference, `exponent != 1` and non-integer weights are rejected.

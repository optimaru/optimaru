# food-packaging-cpsat

CP-SAT alternative for line sequencing, cleanup operators and due-date
objectives. Constraint lists use replacement semantics.

Plugins subclass the `food_packaging.constraints` specs of the same name
(`MaxEndTimeCost`, `AfterIdealEndTimeCost`, `SquaredMakespanCost`,
`OperatorCleaningConflictCost`, `UnassignedJobsCost`) and restate no defaults:
the default level and weight of every plugin come from
`food_packaging.constraint_info()`, and `catalog()` equals it. The objective
uses the levels `hard`, `medium`, `soft`; the two medium constraints sit on
`medium` at weight 1 like the engine, and `hard=False` demotes a hard or a
medium constraint to `soft` (as the engine does since 0.2.0). Not supported on
CP-SAT: `exponent != 1` and non-integer weights.

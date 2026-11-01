# food-packaging-cpsat

CP-SAT alternative for line sequencing, cleanup operators and due-date
objectives. Constraint lists use replacement semantics.

Plugins subclass the `food_packaging.constraints` specs of the same name
(`MaxEndTimeCost`, `AfterIdealEndTimeCost`, `SquaredMakespanCost`,
`OperatorCleaningConflictCost`, `UnassignedJobsCost`) and restate no defaults:
the default level and weight of every plugin come from
`food_packaging.constraint_info()`, and `catalog()` equals it. The objective
uses the levels `hard`, `medium`, `soft`; the two medium constraints sit on
`medium` at weight 1 like the engine, and `level="hard"`, `"medium"` or `"soft"`
(or a `Level`) moves any pick onto that level with its weight in units of it, as
the engine does (`level="soft"` has demoted a medium constraint to `soft` on both
solvers since 0.2.0). Not supported on CP-SAT: `exponent != 1` and non-integer
weights.

**0.3.0: repeats, `offset` and `clip`.** A constraint may be picked several times
(as specs or as `{"name", "weight", "level", "offset", "clip"}` mappings): each
pick builds its own terms with its own knobs, its level included, keyed `name`,
`name#2`, … like the engine's `constraint_costs()`. Under an offset the end-time
plugins add one term per line (an empty one included, never the pool; at offset 0
they add one per job, which sums to the same per-line raw cost), the makespan plugin
always one per line, and the two globals one term for their total, so an integral
offset is shaped exactly as in the engine, in both modes; a fractional offset is
refused (`offset must be an integer for CP-SAT`). `clip=False` with an offset is refused
on hard with the engine's message (`constraint 'max_end_time_cost': clip=False with a
non-zero offset cannot be hard, ...`); medium folds into soft, as in the engine, so it
takes the literal formula. `tests/test_solve.py` pins several layouts and checks that the CP-SAT
objective, the engine's own score and `food_packaging.constraints.score()` agree per
level under repeated, shifted and re-levelled picks.

# Sports league CP-SAT

CP-SAT round assignment with team/day conflicts, four-or-more home/away runs,
reverse fixtures, travel hops, workday-derby penalties, and lexicographic
hard/soft optimization. Results are re-scored by the shared engine.

One plugin per constraint (`sports_league_cpsat/constraints.py`), each subclassing the
domain spec of the same name (`TeamConflictCost`, `WorkdayClassicMatchesCost`, ...) and
registered in `sports_league.list_constraints()` order. The default level and weight of
every plugin come from the engine's `sports_league.constraint_info()`, so the two solvers
cannot disagree on a default (before 0.2.0 the derby plugin used weight 1 instead of the
engine's 1000). `hard=False` demotes a hard constraint to soft, as in the engine;
`exponent != 1` and non-integer weights are not supported on this path.

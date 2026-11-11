# Sports league scheduling CP-SAT

CP-SAT round assignment with team/day conflicts, four-or-more home/away runs,
reverse fixtures, travel hops, workday-derby penalties, and lexicographic
hard/soft optimization. Results are re-scored by the shared engine.

One plugin per constraint ([`external_solver.sports_league_scheduling.constraints`](https://github.com/optimaru/external-solver/blob/v0.2.3/src/external_solver/sports_league_scheduling/constraints.py)), each subclassing the
domain spec of the same name (`TeamConflictCost`, `WorkdayClassicMatchesCost`, ...) and
registered in `optimaru.sports_league_scheduling.list_constraints()` order. The default level and weight of
every plugin come from the engine's `optimaru.sports_league_scheduling.constraint_info()`, so the two solvers
cannot disagree on a default. `level="soft"` / `level="hard"` moves a term to the soft / hard level,
as in the engine, and `level="medium"` is refused by both (`this use case has no medium
level`); `exponent != 1` and non-integer weights are not supported on this path.

A constraint may be picked several times (as specs or as
`{"name", "weight", "level", "offset", "clip"}` mappings); each pick builds its own terms
with its own knobs, its level included. The three unit constraints add one term per
round position, an empty round included, and the seven globals one term for their
total, so an integral `offset` shapes each round (or the total) exactly as the engine
does in both `clip` modes; a fractional offset is refused (`offset must be an integer
for CP-SAT`). `clip=False` with an offset on a hard pick is refused with the engine's
message when the objective is built.

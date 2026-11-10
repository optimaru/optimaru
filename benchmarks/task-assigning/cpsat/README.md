# task-assigning-cpsat

CP-SAT alternative for task assigning: external-solver's `external_solver.task_assigning`
(this package holds the engine vs CP-SAT comparison and points external-solver at the engine
in this checkout). Its `catalog()` exposes independently selectable constraints/objectives;
passing `constraints` replaces the defaults.

Each plugin subclasses the `optimaru.task_assigning.constraints` spec of the same name
(`RequiredSkillCost`, `SquaredMakespanCost`, `CriticalPriorityTaskEndTimeCost`,
`MajorPriorityTaskEndTimeCost`, `MinorPriorityTaskEndTimeCost`,
`UnassignedTasksCost`) and only builds its expression; default levels and
weights come from `task_assigning.constraint_info()`, so `catalog()` equals the
engine catalog. The objective uses the engine's three levels
`("hard", "medium", "soft")`: `required_skill_cost` hard, `unassigned_tasks_cost`
medium (the Bendable soft[0]), the rest soft, with `squared_makespan_cost` at its
engine default weight 10^7 so the Bendable soft[1] stays above soft[2]. The
returned model splits the soft level back into `score_soft1` / `score_soft2` the
way the engine's Score adapter does. `level="hard"`, `"medium"` or `"soft"` (or a
`Level`) moves any pick onto that level with its weight in units of it, as in the
engine: `level="soft"` demotes a hard or medium term to soft, and `level="medium"`
puts a hard or soft term on the Bendable soft[0], below hard. Not supported on
CP-SAT: `exponent != 1` and non-integer weights.

**Repeats, `offset` and `clip`.** A constraint may be picked several times (as
specs or as `{"name", "weight", "level", "offset", "clip"}` mappings): each pick builds
its own terms with its own knobs, its level included, keyed `name`, `name#2`, … like
the engine's `constraint_costs()`. Every plugin adds one term per employee (an idle one
included; the priority end-time plugins add one per task at offset 0, which sums to the
same per-employee raw cost) or, for `unassigned_tasks_cost`, one for its total, so an
integral offset is shaped exactly as in the engine, in both modes; a fractional offset is
refused (`offset must be an integer for CP-SAT`). The objective is built with the engine's
medium fold (`medium_fold="below_hard"`, read off `constraint_costs()`), so `clip=False`
with an offset is refused on hard and on medium with the engine's messages (`constraint
'required_skill_cost': clip=False with a non-zero offset cannot be hard, ...` and
`constraint 'unassigned_tasks_cost': clip=False with a non-zero offset cannot be medium
here, ...`). External-solver's `tests/task_assigning/test_solve.py` pins several layouts and checks
that the CP-SAT objective, `optimaru.task_assigning.constraints.score()` and (wherever every task
is assigned) the engine's own score agree per level under repeated, shifted and
re-levelled picks.

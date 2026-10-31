# task-assigning-cpsat

CP-SAT alternative for task assigning. `catalog()` exposes independently
selectable constraints/objectives; passing `constraints` replaces the defaults.

Each plugin subclasses the `task_assigning.constraints` spec of the same name
(`RequiredSkillCost`, `SquaredMakespanCost`, `CriticalPriorityTaskEndTimeCost`,
`MajorPriorityTaskEndTimeCost`, `MinorPriorityTaskEndTimeCost`,
`UnassignedTasksCost`) and only builds its expression; default levels and
weights come from `task_assigning.constraint_info()`, so `catalog()` equals the
engine catalog. The objective uses the engine's three levels
`("hard", "medium", "soft")`: `required_skill_cost` hard, `unassigned_tasks_cost`
medium (the Bendable soft[0]), the rest soft, with `squared_makespan_cost` at its
engine default weight 10^7 so the Bendable soft[1] stays above soft[2]. The
returned model splits the soft level back into `score_soft1` / `score_soft2` the
way the engine's Score adapter does. `hard=False` demotes a hard or medium term
to soft, as in the engine. Not supported on CP-SAT: `exponent != 1` and
non-integer weights.

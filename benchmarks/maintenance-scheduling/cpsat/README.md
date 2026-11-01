# Maintenance scheduling CP-SAT

CP-SAT backend for `MaintenanceScheduleModel`. It models crew/date choices,
business-day end dates, crew and shared-tag overlap, date bounds, pinned jobs,
and the lexicographic hard/soft objective. Results are re-scored by the engine.

The constraint plugins (`maintenance_scheduling_cpsat/constraints.py`) subclass the
domain specs (`MinStartDateCost`, `MaxEndDateCost`, `BeforeIdealEndDateCost`,
`AfterIdealEndDateCost`, `CrewConflictCost`, `TagConflictCost`) and are registered in
`list_constraints()` order. Their default level and weight come from the engine's
`maintenance_scheduling.constraint_info()` (since 0.2.0; before, the 1,000,000 and 1,000
soft weights were copied here by hand), so `catalog()` equals the engine catalog and
`level="soft"` demotes to soft in both solvers. Not supported on this exact path (rejected):
`exponent != 1` and non-integer weights.

## Engine comparison

The comparison runner is owned by this use case. It builds deterministic
instances, gives both backends the same solve budget, and re-scores both answers
with the engine evaluator.

```sh
uv run maintenance-scheduling-compare --seconds 10
uv run maintenance-scheduling-compare --instance small --seconds 2 --json
```

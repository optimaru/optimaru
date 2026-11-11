# Maintenance scheduling CP-SAT

CP-SAT backend for `MaintenanceScheduleModel`. It models crew/date choices,
business-day end dates, crew and shared-tag overlap, date bounds, pinned jobs,
and the lexicographic hard/soft objective. Results are re-scored by the engine.

The constraint plugins ([`external_solver.maintenance_scheduling.constraints`](https://github.com/optimaru/external-solver/blob/v0.2.3/src/external_solver/maintenance_scheduling/constraints.py)) subclass the
domain specs (`MinStartDateCost`, `MaxEndDateCost`, `BeforeIdealEndDateCost`,
`AfterIdealEndDateCost`, `CrewConflictCost`, `TagConflictCost`) and are registered in
`list_constraints()` order. Their default level and weight come from the engine's
`maintenance_scheduling.constraint_info()`, so `catalog()` equals the engine catalog and
`level="soft"` / `level="hard"` moves a term to the soft / hard level in both solvers;
`level="medium"` is refused by both (`this use case has no medium level`). Not supported
on this exact path (rejected): `exponent != 1` and non-integer weights.

A constraint may be picked several times (as specs or as
`{"name", "weight", "level", "offset", "clip"}` mappings); each pick builds its own terms
with its own knobs, its level included. The four date constraints add one term per (crew,
start date) cell, an empty cell included, and the two conflict constraints one term for their
total, so an integral `offset` shapes each cell (or the total) exactly as the engine does
in both `clip` modes; a fractional offset is refused (`offset must be an integer for
CP-SAT`). `clip=False` with an offset on a hard pick is refused with the engine's message
when the objective is built.

## Engine comparison

The comparison runner is owned by this use case. It builds deterministic
instances, gives both backends the same solve budget, and re-scores both answers
with the engine evaluator.

```sh
uv run maintenance-scheduling-compare --seconds 10
uv run maintenance-scheduling-compare --instance small --seconds 2 --json
```

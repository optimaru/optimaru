# Maintenance scheduling CP-SAT

CP-SAT backend for `MaintenanceScheduleModel`. It models crew/date choices,
business-day end dates, crew and shared-tag overlap, date bounds, pinned jobs,
and the lexicographic hard/soft objective. Results are re-scored by the engine.

## Engine comparison

The comparison runner is owned by this use case. It builds deterministic
instances, gives both backends the same solve budget, and re-scores both answers
with the engine evaluator.

```sh
uv run maintenance-scheduling-compare --seconds 10
uv run maintenance-scheduling-compare --instance small --seconds 2 --json
```

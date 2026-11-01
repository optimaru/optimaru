# Project job scheduling CP-SAT

Multi-mode RCPSP formulation with precedence-derived dates, renewable
cumulative resources, non-renewable budgets, pinned mode/delay choices,
project tardiness, and makespan. The returned solution is re-scored by the
shared engine.

## Engine comparison

The comparison runner is owned by this use case. It builds deterministic
instances, gives both backends the same solve budget, and re-scores both answers
with the engine evaluator.

```sh
uv run project-job-compare --seconds 10
uv run project-job-compare --instance demo --seconds 2 --json
```

## Constraints

Plugins subclass the `project_job.constraints` specs of the same name
(`NonRenewableResourceCapacityCost`, `RenewableResourceCapacityCost`,
`ProjectDelayCost`, `MakespanCost`) and restate no defaults: the default level
and weight of every plugin come from `project_job.constraint_info()`, and
`catalog()` equals it. The objective uses the levels `hard`, `medium`, `soft`;
`project_delay_cost` sits on `medium` at weight 1 like the engine, and
`level="hard"`, `"medium"` or `"soft"` (or a `Level`) moves any pick onto that
level with its weight in units of it, as the engine does (`level="soft"` has
demoted a hard or a medium constraint to `soft` since 0.2.0; before, CP-SAT moved
a demoted hard constraint to `medium`). Not supported on CP-SAT: `exponent != 1`
and non-integer weights.

**0.3.0: repeats, `offset` and `clip`.** A constraint may be picked several times (as
specs or as `{"name", "weight", "level", "offset", "clip"}` mappings): each pick builds
its own terms with its own knobs, its level included, keyed `name`, `name#2`, … like the
engine's `constraint_costs()`. All four constraints are global, so every plugin adds one
term for its total and an integral offset is shaped exactly as in the engine, in both
modes; a fractional offset is refused (`offset must be an integer for CP-SAT`).
`tests/test_solve.py` pins every mode and delay of a two-project instance and checks
that the CP-SAT objective, the engine's own score and `project_job.constraints.score()`
agree per level under repeated, shifted and re-levelled picks.

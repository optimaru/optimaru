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
`level="soft"` demotes a hard or a medium constraint to `soft` (as the engine
does since 0.2.0; before, CP-SAT moved a demoted hard constraint to `medium`).
Not supported on CP-SAT: `exponent != 1` and non-integer weights.

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

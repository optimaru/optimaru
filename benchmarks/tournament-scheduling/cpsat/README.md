# Tournament scheduling CP-SAT

CP-SAT backend for day/team assignment. Hard day collisions and
unavailability are optimized first, followed by squared team-load and
confrontation-load balancing. The final canonical unfairness score is computed
by the shared engine evaluator.

## Engine comparison

The comparison runner is owned by this use case. It builds deterministic
instances, gives both backends the same solve budget, and re-scores both answers
with the engine evaluator.

```sh
uv run tournament-scheduling-compare --seconds 10
uv run tournament-scheduling-compare --instance demo --seconds 2 --json
```

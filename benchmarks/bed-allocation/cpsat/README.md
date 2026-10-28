# Bed allocation CP-SAT

OR-Tools CP-SAT formulation using the canonical `BedPlanModel`. It preserves
the hard/medium/soft ordering, supports the no-bed sentinel, and re-scores the
result with the shared engine evaluator.

## Engine comparison

The comparison runner is owned by this use case. It builds deterministic
instances, gives both backends the same solve budget, and re-scores both answers
with the engine evaluator.

```sh
uv run bed-allocation-compare --seconds 10
uv run bed-allocation-compare --instance demo --seconds 2 --json
```

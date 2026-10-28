# Flight crew CP-SAT

CP-SAT backend for the canonical flight-crew model. It optimizes skills,
availability, overlapping duties, chronological airport transfers, home-base
starts/ends, and pinned assignments. The engine evaluator supplies the final
reported score.

## Engine comparison

The comparison runner is owned by this use case. It builds deterministic
instances, gives both backends the same solve budget, and re-scores both answers
with the engine evaluator.

```sh
uv run flight-crew-compare --seconds 10
uv run flight-crew-compare --instance demo --seconds 2 --json
```

# Box sizing CP-SAT

The OR-Tools CP-SAT model of the canonical `optimaru.box_sizing.models` contract, and its
comparison with the engine in this checkout. The model lives here
([`box_sizing_cpsat/model.py`](box_sizing_cpsat/model.py)), not in external-solver.

```python
import box_sizing_cpsat  # noqa: F401 - checks that optimaru.box_sizing is the checkout's
from box_sizing_cpsat.model import solve

solved = solve(problem, time_limit_seconds=10, random_seed=0)   # a BoxSizingProblemModel in, a solved one out
```

## The model

Parcels that fit the same candidate boxes pay the same for a box up to a constant, so they
form one group, and a group ships in one box. One boolean per (group, box) pair that fits
(by size, in any rotation, and by `max_weight`) and one per box that says the box is in use:

- every group ships in exactly one box that takes it;
- a group ships only in a box in use, and a required box is in use;
- at most `max_box_types` boxes are in use;
- minimise, over the pairs chosen, the summed `quantity x (box volume - parcel volume) +
  quantity x box cost` of the group's parcels.

The three hard constraints (`parcel_fit_cost`, `box_weight_cost`, `box_type_limit_cost`) are
structural: a pair that does not fit has no variable and the limit is a linear constraint, so
a solution of the model has hard 0 and `level="soft"` has no meaning there. The objective is
`void_volume_cost` + `box_cost` at their default weights; `box_type_cost` (off by default)
is not modelled, and the model takes no constraint selection. A box `cost` must be a multiple
of 0.001: the costs are scaled by the smallest power of ten that makes them whole.

`tests/test_model.py` checks the model's objective against the engine's soft score of the
same assignment, and both solvers against an enumeration of every set of sizes on instances
small enough to enumerate.

## Engine comparison

The comparison runner is owned by this use case. It builds deterministic
instances, gives both backends the same solve budget, and re-scores both answers
with the engine evaluator.

```sh
uv run box-sizing-compare --seconds 10
uv run box-sizing-compare --instance demo --seconds 2 --json
```

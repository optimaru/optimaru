# max_cut (Python)

The pybind11 package of the max-cut use case (`../cpp`). Splits the vertices of a
weighted graph into two sides — the side of each vertex (0 or 1) is the only
planning variable; a vertex in `fixed_partition` is pinned.

Score (HardSoftScore), at the default weights:

- hard = -(fixed vertices off their side + split same-side pairs + different-side
  pairs left together + side-size shortfall/excess) — four constraints, hard x1 each
- soft = +(cut weight) — `cut_weight_cost`, a reward (raw cost `-(cut weight)`),
  soft x1; it is always soft (`level="hard"` raises)
- `partition_imbalance_cost` (`abs(n0 - n1)`) is an opt-in objective: soft, default
  weight 0

The problem carries no weights. Pick, re-weight or flip constraints with
`max_cut.constraints`, and read the defaults with `max_cut.constraint_info()`:

```python
import max_cut
from max_cut.constraints import CutWeightCost, FixedPartitionCost, PartitionImbalanceCost, get_constraints, score

max_cut.constraint_info()
# [ConstraintInfo(name='fixed_partition_cost', default_level='hard', default_weight=1.0,
#                 level_flippable=True, reward=False, default_enabled=True), ...,
#  ConstraintInfo(name='partition_imbalance_cost', default_level='soft', default_weight=0.0,
#                 level_flippable=True, reward=False, default_enabled=False),
#  ConstraintInfo(name='cut_weight_cost', default_level='soft', default_weight=1.0,
#                 level_flippable=False, reward=True, default_enabled=True)]
max_cut.solve(
    problem,
    time_limit_seconds=1.0,
    constraints=[*get_constraints()[:4], PartitionImbalanceCost(weight=2), CutWeightCost()],
)
raw = max_cut.constraint_costs(problem)   # {name: {"costs": [side 0, side 1] or [total], "weight": ..., ...}}
score(raw), score(raw, [FixedPartitionCost(exponent=2.0)])
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping (clip=True: only the imbalance beyond 2 is squared).
score(raw, [PartitionImbalanceCost(weight=1), PartitionImbalanceCost(weight=5, exponent=2, offset=2)])
```

The four unit constraints (fixed, same-side, different-side, size bounds) report one
raw cost per side and `exponent` (with `offset` / `clip`, 0.3.0) shapes each side; the
imbalance and the cut report one total. `constraint_costs(problem, picks)` scores a pick
list, the second pick of a name keyed `<name>#2`. `calculate_score` / `constraint_costs` need a complete partition (a partial
one raises `ValueError`). 0.2.0 renamed every constraint (no aliases), replaced the
per-term lists of `constraint_costs()` by the standard dict, shapes each side instead
of each fixed vertex / pair, rounds hard per side, refuses `level="hard"` on the reward
and partial partitions, and replaced the simulated-annealing `SolverConfig` knobs
(`max_iterations`, `stagnation_limit`, `temperature`, `cooling_rate`) by the engine's
(`spent_limit_seconds`, `step_count_limit`, ...); see the
[use-case README](../README.md#020).

## Install / test

```bash
uv sync --all-groups --all-extras
uv run pytest -q
```

## Usage

```python
import max_cut
from max_cut.models import MaxCutProblemModel

problem = MaxCutProblemModel.model_validate(data)            # the canonical JSON
max_cut.solve(problem, time_limit_seconds=5.0, random_seed=42)   # in place: partition, cut, score
print(problem.score_hard, problem.score_soft, problem.cut_weight, problem.partition)

core = problem.to_core()                                     # the engine object
max_cut.solve(core, time_limit_seconds=5.0, random_seed=42)
print(core.score, core.assignment(), core.cut_weight())
```

For finer control build `max_cut.SolverConfig()` (time budget, `step_count_limit`,
multi-start, ALNS knobs, move weights, the `on_new_best(step, score)` callback,
`best_score_limit` early stop, ...) and call `max_cut.Solver(cfg).solve(core)` or
`max_cut.solve(problem, config=cfg)`. A zero budget (`spent_limit_seconds = 0`)
returns the start partition (vertex `i` on side `i % 2`, fixed vertices on their
side), or a warm-start seed, unchanged. Under exact size bounds
(`min_partition_size == max_partition_size`) the search only swaps sides, so the
start's side sizes are kept, also without a seed: when fixed vertices unbalance the
`i % 2` start, the result stays infeasible even if a feasible partition exists (as in
0.1.5). Seed a partition of the right size with `with_initial_solution` instead.

Note: list attributes of the engine object return copies — assign whole lists and
write the partition in bulk with `core.set_assignment([...])`.

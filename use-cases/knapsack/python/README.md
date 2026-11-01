# knapsack (Python)

The pybind11 package of the knapsack use case (`../cpp`). Chooses the items to
carry — `Item.selected` is the only planning variable (1 = selected, 0 = not).

Score (HardSoftScore), at the default weights:

- hard = -(weight over capacity + resource overflow + unselected mandatory entries
  + selected forbidden entries + selection-group shortfall/excess + unmet item
  dependencies) — six constraints, hard x1 each
- soft = +(selected value) — `selected_value_cost`, a reward (raw cost
  `-(selected value)`), soft x1; it is always soft (`level="hard"` raises)

The problem carries no weights. Pick, re-weight or flip constraints with
`knapsack.constraints`, and read the defaults with `knapsack.constraint_info()`:

```python
import knapsack
from knapsack.constraints import MandatoryItemsCost, SelectedValueCost, get_constraints, score

knapsack.constraint_info()
# [ConstraintInfo(name='knapsack_capacity_cost', default_level='hard', default_weight=1.0,
#                 level_flippable=True, reward=False, default_enabled=True), ...,
#  ConstraintInfo(name='selected_value_cost', default_level='soft', default_weight=1.0,
#                 level_flippable=False, reward=True, default_enabled=True)]
knapsack.solve(problem, time_limit_seconds=1.0, constraints=[*get_constraints()[:-1], SelectedValueCost(weight=2)])
raw = knapsack.constraint_costs(problem)   # {name: {"costs": [total], "weight": ..., ...}}
score(raw), score(raw, [MandatoryItemsCost(exponent=2.0)])
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping of the total (clip=True: only the misses beyond the first count).
score(raw, [MandatoryItemsCost(), MandatoryItemsCost(exponent=2, offset=1)])
```

Every knapsack constraint is global: `constraint_costs()` reports one raw total per
constraint and `exponent` (with `offset` / `clip`, 0.3.0) shapes that total, a raw 0
included. `clip=False` with an offset is refused on hard, where hard 0 must mean no
violation: `ValueError: constraint 'mandatory_items_cost': clip=False with a non-zero offset
cannot be hard, because hard 0 must mean no violation; use clip=True (a threshold) or
another level`. On soft (and on the soft reward) it is allowed.
`constraint_costs(problem, picks)` scores a pick list, the second pick of a name keyed
`<name>#2`. `level` is None (the default level), "hard" or "soft" (or `Level.HARD` /
`Level.SOFT`); the domain has no medium level, so "medium" raises `ValueError`, and a
repeat carries its own level. 0.2.0 renamed every constraint (no
aliases), replaced the per-term lists of `constraint_costs()` by the standard dict,
shapes each constraint's total instead of each term, rounds hard per constraint, refuses
`level="hard"` on the reward, and replaced the simulated-annealing `SolverConfig` knobs
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
import knapsack
from knapsack.models import KnapsackProblemModel

problem = KnapsackProblemModel.model_validate(data)          # the canonical JSON
knapsack.solve(problem, time_limit_seconds=5.0, random_seed=42)   # in place: selection, totals, score
print(problem.score_hard, problem.score_soft, problem.total_weight, problem.total_value)

core = problem.to_core()                                     # the engine object
knapsack.solve(core, time_limit_seconds=5.0, random_seed=42)
print(core.score, core.assignment(), core.total_value())
```

For finer control build `knapsack.SolverConfig()` (time budget, `step_count_limit`,
multi-start, ALNS knobs, move weights, nearby swaps, the `on_new_best(step, score)`
callback, `best_score_limit` early stop, ...) and call `knapsack.Solver(cfg).solve(core)`
or `knapsack.solve(problem, config=cfg)`. A zero budget (`spent_limit_seconds = 0`)
returns the greedy construction, or a warm-start seed, unchanged.

Note: `core.items[i]` returns a copy — write the selection in bulk with
`core.set_assignment([...])`.

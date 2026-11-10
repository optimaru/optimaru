# semiconductor_scheduling (Python)

The pybind11 package of the semiconductor fab use case (`optimaru/semiconductor_scheduling/cpp`, namespace `fab`). The
Pydantic models (`optimaru.semiconductor_scheduling.models`) are the primary API: the web demo and
the server speak them, and `solve` writes its result back into the model it is given.

Score, at the default weights:

- hard = -(the raw costs of the 14 hard constraints, weight 1 each)
- soft = -(machine_preference_cost + min_batch_size_cost + machine_load_imbalance_cost
  + 10 x weighted_tardiness_cost + cycle_time_cost + makespan_cost)

The problem carries no weights. Pick, re-weight or flip constraints with
`optimaru.semiconductor_scheduling.constraints`, and read the defaults with `fab.constraint_info()`:

```python
from optimaru import semiconductor_scheduling as fab
from optimaru.semiconductor_scheduling.constraints import (
    MakespanCost, MaxQueueTimeCost, WeightedTardinessCost, get_constraints, score,
)

fab.constraint_info()
# [ConstraintInfo(name='machine_eligibility_cost', default_level='hard', default_weight=1.0,
#                 level_flippable=True, reward=False, default_enabled=True), ...]
picks = [*get_constraints()[:7], MaxQueueTimeCost(level="soft"), *get_constraints()[8:17],
         WeightedTardinessCost(weight=50), *get_constraints()[18:]]
fab.solve(problem, time_limit_seconds=5, constraints=picks)
raw = fab.constraint_costs(problem)        # raw totals, re-priced in pure Python:
score(raw), score(raw, picks), score(raw, [WeightedTardinessCost(exponent=2.0)])
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping of the total (clip=True: only the minutes beyond 480 count).
score(raw, [MakespanCost(), MakespanCost(weight=100, offset=480)])
fab.constraint_items(problem)              # the per-item lists behind each total
```

Every constraint is global: `exponent`, `offset` and `clip` shape its total, a raw
0 included. `clip=False` with an offset is refused on hard, where hard 0 must mean no
violation: `ValueError: constraint 'machine_capacity_cost': clip=False with a non-zero offset
cannot be hard, because hard 0 must mean no violation; use clip=True (a threshold) or another
level`. On soft it is allowed. `constraint_costs(problem, picks)` scores a pick list, the
second pick of a name keyed
`<name>#2`. `level` is None (the default level), "hard" or "soft" (or `Level.HARD` /
`Level.SOFT`); the domain has no medium level, so "medium" raises `ValueError`, and a
repeat carries its own level.

## Install / test

```bash
OPTIMARU_USE_CASES=semiconductor_scheduling uv sync --python 3.14   # builds only this use case's C++ extension
uv run --python 3.14 --with pytest python -m pytest -q tests
```

`tests/reference_semiconductor_scheduling.py` is a pure-Python reference scorer and
decoder: `tests/test_reference_parity.py` checks the C++ raw costs, per-item lists, default
score, decoded schedules and seed states against it on every sample and on 200 seeded
random instances.

## Usage

```python
from optimaru import semiconductor_scheduling as fab
from optimaru.semiconductor_scheduling.models import SemiconductorScheduleModel

problem = SemiconductorScheduleModel.model_validate_json(open("fab.json").read())
score = fab.solve(problem, time_limit_seconds=5.0, random_seed=42)   # written back into `problem`
hard, soft = score                                                   # a Score is also a (hard, soft) pair
print(problem.makespan, problem.total_weighted_tardiness, problem.assignments[0])

core = problem.to_core()                     # the engine object (what the server solves)
fab.solve(core, time_limit_seconds=5.0)      # in place
solved = SemiconductorScheduleModel.from_core(core)
```

For finer control build `fab.SolverConfig()` and pass `config=`:

| knob | default | meaning |
|---|---|---|
| `spent_limit_seconds` | 30 | wall clock; 0 returns the start as is |
| `step_count_limit`, `unimproved_step_count_limit`, `unimproved_spent_limit_seconds` | 0 (off) | early exits |
| `random_seed`, `parallel_solver_count` | 0, 1 | independent threads with seed + i; the strictly best wins |
| `acceptor_type` | `SIMULATED_ANNEALING` | the engine's acceptor (`LATE_ACCEPTANCE`, `HILL_CLIMBING`, ...) |
| `simulated_annealing_starting_temperature`, `simulated_annealing_temperature_fraction` | (0, 0) = auto, 0.01 | auto: hard 0, soft = 1% of the start's \|soft\|, cooling over the time limit (or over `step_count_limit` when that is further along). A seeded `step_count_limit` run repeats only while its step fraction leads the time fraction: give it a time limit generous enough for the step budget |
| `late_acceptance_size` | 0 (= clamp(16 x rows, 400, 3200)) | for `LATE_ACCEPTANCE` |
| `machine_move_probability` | 0.55 | share of change moves that switch a machine (else a priority shift of 1..8) |
| `priority_swap_moves`, `custom_move_probability` | True, 0.45 | the priority swap of two operations, and its gate |
| `change_move_weight`, `swap_move_weight` | 1, 0 | the engine's assignment-shape move mix |
| `ruin_recreate`, `ruin_recreate_*` | off | the engine's ALNS (a recreate decodes once per candidate value) |
| `best_score_limit`, `full_assert`, `on_new_best` | | stop early, verify every move, `on_new_best(step, score)` |

## Starting from an existing solution

`fab.with_initial_solution(problem, solution)` returns a copy whose search starts from
`solution` (a solved model or its JSON dict). A complete seed is scored as supplied and
returned unchanged unless the search finds a strictly better schedule; a partial seed
keeps its machines and relative order and is decoded.

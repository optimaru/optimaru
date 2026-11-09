# assembly_line_balancing (Python)

The pybind11 package of the assembly-line-balancing use case (`../cpp`). Puts every
task on one station of a paced line — `Task.station` is the only planning variable
(station index, -1 = unassigned).

Score, at the default weights:

- hard = -(station capacity overflow + reversed precedences + stations beyond
  `max_stations` + station-domain distance + missed fixed stations + forbidden
  stations hit + extra stations of same-station groups + different-station pairs
  together) — eight constraints, hard x1 each
- soft = -(stations used) — `used_stations_cost` soft x1; `workload_imbalance_cost`
  (max - min load of the used stations) is an opt-in objective, soft x0

The problem carries no weights. Pick, re-weight or flip constraints with
`assembly_line_balancing.constraints`, and read the defaults with
`alb.constraint_info()`. `level` is `"hard"` or `"soft"` (`Level.HARD` / `Level.SOFT`);
the domain has no medium level, so `level="medium"` raises `ValueError`:

```python
import assembly_line_balancing as alb
from assembly_line_balancing.constraints import UsedStationsCost, WorkloadImbalanceCost, get_constraints, score

alb.constraint_info()
# [ConstraintInfo(name='station_capacity_cost', default_level='hard', default_weight=1.0,
#                 level_flippable=True, reward=False, default_enabled=True), ...,
#  ConstraintInfo(name='workload_imbalance_cost', default_level='soft', default_weight=0.0,
#                 level_flippable=True, reward=False, default_enabled=False)]
alb.solve(problem, time_limit_seconds=1.0, constraints=[*get_constraints()[:-1], WorkloadImbalanceCost(weight=1)])
raw = alb.constraint_costs(problem)       # raw costs per station (one total for a global)
score(raw), score(raw, [UsedStationsCost(exponent=2.0)])
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping (clip=True: only the used stations beyond the fourth count).
score(raw, [UsedStationsCost(), UsedStationsCost(weight=10, offset=4)])
```

`exponent` shapes each station's raw cost for the per-station constraints and the single
total for the globals (`task_precedence_cost`, `same_station_groups_cost`,
`used_stations_cost`, `workload_imbalance_cost`); the keyword-only `offset` / `clip`
shift that shaping at the same granularity (0.3.0, see the
[use-case README](../README.md#030)). `clip=False` shapes `x - offset` on every evaluated
station, an empty one included (per station, not over the sum), so it is refused on hard,
where hard 0 must mean no violation: `ValueError: constraint 'station_capacity_cost':
clip=False with a non-zero offset cannot be hard, because hard 0 must mean no violation; use
clip=True (a threshold) or another level`. On soft it is allowed.

0.2.0 renamed every constraint (no aliases), replaced the simulated-annealing
`SolverConfig` knobs (`max_iterations`, `stagnation_limit`, `temperature`,
`cooling_rate`) by the engine's (`spent_limit_seconds`, `step_count_limit`, ...), and now
charges an unassigned task nothing; see the [use-case README](../README.md#020).

## Install / test

```bash
uv sync --all-groups --all-extras
uv run pytest -q
```

## Usage

```python
import assembly_line_balancing as alb
from assembly_line_balancing.models import AssemblyLineBalancingProblemModel

problem = AssemblyLineBalancingProblemModel.model_validate(data)   # the canonical JSON
alb.solve(problem, time_limit_seconds=5.0, random_seed=42)         # in place: stations, loads, score
print(problem.score_hard, problem.score_soft, problem.stations_used, problem.station_loads)

core = problem.to_core()                                           # the engine object
alb.solve(core, time_limit_seconds=5.0, random_seed=42)
print(core.score, core.assignment(), core.station_loads())
```

For finer control build `alb.SolverConfig()` (time budget, `step_count_limit`,
multi-start, ALNS knobs, `close_station_moves`, `value_recommender_probability`, the
`on_new_best(step, score)` callback, `best_score_limit` early stop, ...) and call
`alb.Solver(cfg).solve(core)` or `alb.solve(problem, config=cfg)`. A zero budget
(`spent_limit_seconds = 0`) returns the construction, or a complete warm-start seed,
unchanged.

Note: `core.tasks[i]` returns a copy — write stations in bulk with
`core.set_assignment([...])`.

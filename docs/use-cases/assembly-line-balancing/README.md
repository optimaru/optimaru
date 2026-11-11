# Assembly-line balancing

Simple assembly-line balancing (SALBP-1) with zoning and station domains, on
this repository's C++ engine (`optimaru/cpp/optimaru/`): put every task on one
station of a paced line so that no station exceeds the cycle time, precedences
run forward, and as few stations as possible are opened. The base model is
the standard SALBP-1 formulation (Scholl 1999; Otto, Otto & Scholl 2013).
The exact CP-SAT reference is `external_solver.assembly_line_balancing`
([external-solver](https://github.com/optimaru/external-solver)), run against this engine by
[`benchmarks/assembly-line-balancing/cpsat`](../../../benchmarks/assembly-line-balancing/cpsat).

## Usage

```sh
OPTIMARU_USE_CASES=assembly_line_balancing uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/assembly_line_balancing
```

```python
from optimaru import assembly_line_balancing as alb
from optimaru.assembly_line_balancing.models import AssemblyLineBalancingProblemModel, PrecedenceModel, TaskModel

problem = AssemblyLineBalancingProblemModel(
    cycle_time=5,
    tasks=[TaskModel(id="A", duration=3), TaskModel(id="B", duration=3), TaskModel(id="C", duration=2)],
    precedences=[PrecedenceModel(before="A", after="B")],
)
problem = alb.solve(problem, time_limit_seconds=1.0, random_seed=42)   # the model: returns a new, solved model
print({task.id: task.station for task in problem.tasks}, problem.stations_used)

core = problem.to_core()                                            # or the engine object
alb.solve(core, time_limit_seconds=1.0, random_seed=42)
print(AssemblyLineBalancingProblemModel.from_core(core).station_loads)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`, `full_assert`, `on_new_best`, a full `config` (`alb.SolverConfig`) and
`constraints`:

```python
from optimaru.assembly_line_balancing.constraints import UsedStationsCost, WorkloadImbalanceCost, get_constraints, score

alb.constraint_info()          # [ConstraintInfo(name='station_capacity_cost', default_level='hard', ...), ...]
problem = alb.solve(problem, time_limit_seconds=1.0, constraints=[*get_constraints()[:-1], WorkloadImbalanceCost(weight=1)])
raw = alb.constraint_costs(problem)                 # raw costs, re-priced in pure Python:
score(raw), score(raw, [*get_constraints()[:-1], WorkloadImbalanceCost(weight=2)])
score(raw, [UsedStationsCost(), UsedStationsCost(weight=10, offset=4)])   # a repeat, shifted
```

## Domain (assignment shape)

- `Task.station` — the only decision variable: a station index (0-based), `-1` =
  unassigned. Stations are not bounded above by the facts: a solve freezes the
  value range at `max(task count, 1 + max station)` after its construction
  (`problem.station_count`; CP-SAT searches `0..n-1`).
- Facts: `cycle_time`; per task `duration`, `min_station`, `max_station`,
  `fixed_station` (a penalty, not a pin), `forbidden_stations`; `precedences`
  (before/after), `max_stations`, `same_station_groups`, `different_station_pairs`.
  The engine object refuses, like the pydantic model (a `ValueError` naming
  the field): `cycle_time` or a `duration` <= 0, `max_stations` < 1, a
  `min_station` or `fixed_station` < 0 and a `max_station` below `min_station`.
  The pydantic model also requires station-domain indices below the task count.
- `warm_start` — start from the stations on the tasks (tasks at `-1` go through the
  construction); a plain solve ignores input stations. Every solve resets it.

## Constraints

The score is the engine's `Score` type as it is, with no mapping.

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) | Unit |
|---|---|---|---|---|---|
| `station_capacity_cost` | `StationCapacityCost` | hard | 1 | `max(0, load - cycle_time)` | per station |
| `task_precedence_cost` | `TaskPrecedenceCost` | hard | 1 | arcs `(before, after)` with `station(before) > station(after)` (a duplicate arc counts per copy) | global |
| `max_stations_cost` | `MaxStationsCost` | hard | 1 | per task `max(0, station - max_stations + 1)` | per station |
| `task_station_bounds_cost` | `TaskStationBoundsCost` | hard | 1 | per task `max(0, min_station - station) + max(0, station - max_station)` | per station |
| `fixed_task_stations_cost` | `FixedTaskStationsCost` | hard | 1 | 1 per task off its `fixed_station` | per station |
| `forbidden_task_stations_cost` | `ForbiddenTaskStationsCost` | hard | 1 | 1 per task on one of its `forbidden_stations` (a station listed twice counts once) | per station |
| `same_station_groups_cost` | `SameStationGroupsCost` | hard | 1 | per group `max(0, distinct stations of its members - 1)` | global |
| `different_station_pairs_cost` | `DifferentStationPairsCost` | hard | 1 | 1 per pair sharing a station (a self-pair always) | per station |
| `used_stations_cost` | `UsedStationsCost` | soft | 1 | stations with a load > 0 (the SALBP-1 objective) | global |
| `workload_imbalance_cost` | `WorkloadImbalanceCost` | soft | **0** (opt-in) | max - min load over the used stations | global |

Every constraint is flippable (`level="hard"` / `level="soft"`, or `Level.HARD` /
`Level.SOFT`; `level="medium"` raises `ValueError`, the domain has no medium level),
none is a reward, and all but `workload_imbalance_cost` are scored by default
(`default_enabled`);
`assembly_line_balancing.constraint_info()` reports exactly this table from the C++
components (`kDefaultLevel` / `kDefaultWeight`). An unassigned task (`station = -1`)
contributes nothing (the engine convention): an arc, pair or group member counts
only while its tasks are assigned.

### Scoring

- **Exponent granularity**: the engine shapes a unit constraint per station and a
  global on its total. `max_stations_cost`, `task_station_bounds_cost`,
  `fixed_task_stations_cost`, `forbidden_task_stations_cost` and
  `different_station_pairs_cost` are shaped per station, `same_station_groups_cost`
  on its total. CP-SAT rejects any exponent other than 1.
- Hard contributions are rounded per unit (half away from zero).
- `constraint_costs()` returns the standard dict
  `{name: {"costs", "weight", "exponent", "offset", "clip", "global", "level",
  "reward", "level_flippable", "name", "label", "default_weight", "default_level"}}`
  with per-station lists (`0 .. station_count - 1`).

### Offsets and repeated picks

Every constraint takes the knobs `weight`, `level`, `exponent` and, keyword-only,
`offset` and `clip`, and may be picked several times. A raw cost `x` (per station for
the six per-station constraints, the single total for the four globals — the
granularity of `exponent`) is shaped as `sign(x) * max(0, |x| - offset) ** exponent`
(`clip=True`, the default) or `(x - offset) ** exponent`, sign-preserving
(`clip=False`, which lowers the penalty of every evaluated station below the offset, an
empty one included); `offset=0` is the plain power shaping. `max_stations_cost` and
`forbidden_task_stations_cost` evaluate only their own stations (from `max_stations`
on, and the stations some task forbids), so `clip=False` charges those and no other;
`constraint_costs()` lists them as `"units"`. Both formulas apply per station (or to a
global's one total), not to the sum over the stations. `clip=False` with an offset is
refused on hard (every constraint but `used_stations_cost` and
`workload_imbalance_cost` by default), because hard 0 must mean no violation. Each pick
is its own instance with its own knobs, its level included, and the instances add up:

```python
from optimaru.assembly_line_balancing.constraints import UsedStationsCost, get_constraints

solved = alb.solve(problem, constraints=[
    *get_constraints(),                        # the defaults
    UsedStationsCost(weight=10, offset=4),     # + 10 per used station beyond the fourth
])
```

`alb.constraint_costs(problem, picks)` keys the second pick `used_stations_cost#2` (each
name's instances together, the names in catalog order). The CP-SAT reference takes
integral offsets.

### Python API

- **`SolverConfig`** is the engine's: `spent_limit_seconds`, `step_count_limit`,
  `unimproved_*`, `random_seed`, `parallel_solver_count`, `late_acceptance_size`,
  `full_assert`, `ruin_recreate*`, `change/swap_move_weight`,
  `value_recommender_probability`, `descent_weight`, `custom_move_probability`,
  `close_station_moves`, `best_score_limit`, `on_new_best(step, score)`. Zero budget =
  `spent_limit_seconds = 0` (`step_count_limit = 0` means "no limit"); a bounded search
  = `step_count_limit = N`.
- `solve()` takes the engine object (solved in place and returned) or the model (left as
  it was: a new, solved model is returned, with `warm_start` back to False). The
  score is on the result: `.score` (`.hard`, `.soft`) of the engine object, `score_hard` /
  `score_soft` of the model. `calculate_score(problem, constraints)` returns
  the engine `Score` for the defaults and a `constraints.Score(hard, soft)` named tuple
  for a selection (see `constraints.get_constraints()` / `constraints.score()`). With
  `constraints=...` a solve is single-start (`parallel_solver_count` is ignored), as in
  every C++ package.

## C++

- `optimaru/assembly_line_balancing/cpp/include/alb/domain.hpp` — domain + `optimaru::SolutionTraits` specialization
- `optimaru/assembly_line_balancing/cpp/include/alb/constraints/` — one component per constraint: `station_capacity_cost.hpp`,
  `task_precedence_cost.hpp`, `max_stations_cost.hpp`, `task_station_bounds_cost.hpp`,
  `fixed_task_stations_cost.hpp`, `forbidden_task_stations_cost.hpp`,
  `same_station_groups_cost.hpp`, `different_station_pairs_cost.hpp`,
  `used_stations_cost.hpp`, `workload_imbalance_cost.hpp` (+ `common.hpp`);
  `optimaru/assembly_line_balancing/cpp/include/alb/constraints.hpp` assembles the default set (`defaultConstraints()`) and
  states the catalog order once (`forEachDefaultConstraint`)
- `construction` — level-synchronous Kahn order, first fit on a station pointer that
  never moves down, fixed-station override (identical to the pure-Python reference in
  `tests/assembly_line_balancing/reference_assembly_line_balancing.py`)
- `score_calculator` — full recompute (the reference) and `rawCosts()`
- `incremental_calculator` — before/after hooks, O(degree) per move
- `solver` — engine wrapper (construction + Late Acceptance + adaptive ruin & recreate
  + `CloseStationMove` + the precedence-window value recommender)
- `optimaru/assembly_line_balancing/cpp/src/main.cpp` — a 100-task demo against the trivial lower bound

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPTIMARU_USE_CASES=assembly_line_balancing
cmake --build build -j4
ctest --test-dir build --output-on-failure --no-tests=error
./build/assembly_line_balancing/alb_demo 5
```

`alb_test`: per-constraint hand checks (positive and zero), the construction cases,
incremental == full recompute over random change / swap / unassign / batch moves,
ScoreChecks::Full solves, determinism and multi-start. `alb_plugin_test`: default assembly ==
monolith (score and every raw total), plugin incremental == reset, partial assemblies,
overrides, raw costs by hand per station, the catalog.

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose
search starts from `solution`, a solved model or the same thing as a JSON dict.
Tasks are matched by id; tasks left out or at station -1 are placed by the
normal construction, and a complete seed skips it (with a zero budget,
`spent_limit_seconds = 0`, it comes back unchanged). Any result of `solve` is
accepted, feasible or not: `max_stations`, station domains and `fixed_station`
stay hard penalties. Unknown ids, duplicates, stations below -1 and non-integer
stations raise `ValueError`.

```python
from optimaru.assembly_line_balancing import with_initial_solution

warm = with_initial_solution(problem, problem.model_dump(mode="json"))
warm = alb.solve(warm, time_limit_seconds=1.0, random_seed=42)
print({task.id: task.station for task in warm.tasks}, warm.stations_used)
```

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **assignment** shape; the domain solver that configures the engine is
`optimaru/assembly_line_balancing/cpp/include/alb/solver.hpp` + `optimaru/assembly_line_balancing/cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves (weights in
`Solver::Config`). `CloseStationMove` (empties a station by moving its tasks, in
topological order, into their precedence windows) is defined and registered in
`optimaru/assembly_line_balancing/cpp/src/solver.cpp` (`runEngine`) — use it as the template. A new move is an
`optimaru::MoveOperator<…>` registered with `engine.add_move_operator(...)`; a ruin for
LNS is a `RuinOperator` registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** `optimaru/assembly_line_balancing/cpp/src/construction.cpp` places every task
before the engine starts (the engine only fills what is still unassigned). `alb_test`
and the Python reference-parity tests pin its results.

**Adding a hard or soft cost.** [Adding a constraint, step by step](extending.md) takes one new
constraint through this use case's files in six steps: changed from Python, written as a
component, tested, made cheaper per move, moved to the default fast path and measured.

## Benchmark

[`benchmarks/assembly-line-balancing`](../../../benchmarks/assembly-line-balancing) compares this
use case with OR-Tools CP-SAT: generated lines (`assembly_line_balancing_cpsat.benchmark`) and
the public SALBP archive of Otto et al. (`bench/run_bench.py`, downloaded on demand and
checksum-verified; results in [`bench/RESULTS.md`](../../../benchmarks/assembly-line-balancing/bench/RESULTS.md)).

```sh
cd benchmarks/assembly-line-balancing
uv sync --project cpsat --all-groups
uv run --project cpsat python -m bench.run_bench --dataset large --seconds 60
uv run --project cpsat python -m bench.run_bench --dataset very-large --exact-max-size 0 --seconds 60
```

# Assembly-line balancing

Simple assembly-line balancing (SALBP-1) with zoning and station domains, on
this repository's C++ engine (`include/optimaru/`): put every task on one
station of a paced line so that no station exceeds the cycle time, precedences
run forward, and as few stations as possible are opened. The base SALBP-1
formulation follows the official
[Hexaly assembly-line balancing template](https://www.hexaly.com/docs/last/exampletour/assemblylinebalancing.html).
[`benchmarks/assembly-line-balancing/cpsat`](../../benchmarks/assembly-line-balancing/cpsat)
holds the exact CP-SAT reference.

## Domain (BASIC variable)

- `Task.station` — the only planning variable: a station index (0-based), `-1` =
  unassigned. Stations are not bounded above by the facts: a solve freezes the
  value range at `max(task count, 1 + max station)` after its construction
  (`problem.station_count`; CP-SAT searches `0..n-1`).
- Facts: `cycle_time`; per task `duration`, `min_station`, `max_station`,
  `fixed_station` (a penalty, not a pin), `forbidden_stations`; `precedences`
  (before/after), `max_stations`, `same_station_groups`, `different_station_pairs`.
  The engine object refuses what the pydantic model refuses (a `ValueError` naming
  the field): `cycle_time` or a `duration` <= 0, `max_stations` < 1, a
  `min_station` or `fixed_station` < 0 and a `max_station` below `min_station`.
- `warm_start` — start from the stations on the tasks (tasks at `-1` go through the
  construction); a plain solve ignores input stations. Every solve resets it.

## Constraints (HardSoftScore — the engine score type as-is, no mapping)

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

Every constraint is flippable (`hard=True` / `hard=False`), none is a reward, and
all but `workload_imbalance_cost` are scored by default (`default_enabled`);
`assembly_line_balancing.constraint_info()` reports exactly this table from the C++
components (`kDefaultLevel` / `kDefaultWeight`). An unassigned task (`station = -1`)
contributes nothing (the engine convention): an arc, pair or group member counts
only while its tasks are assigned.

### 0.2.0

The package was a pure-Python reference (simulated annealing over
`constraint_costs()`); it is now the C++ engine with pybind11 bindings, like the
other C++ use cases.

- **Names** (no aliases): `station_capacity` -> `station_capacity_cost`,
  `task_precedence` -> `task_precedence_cost`, `station_count_limit` ->
  `max_stations_cost`, `task_station_bounds` -> `task_station_bounds_cost`,
  `fixed_task_stations` -> `fixed_task_stations_cost`, `forbidden_task_stations` ->
  `forbidden_task_stations_cost`, `same_station_groups` -> `same_station_groups_cost`,
  `different_station_pairs` -> `different_station_pairs_cost`, `minimize_stations` ->
  `used_stations_cost`, `workload_balance` -> `workload_imbalance_cost`; the spec
  classes and module-level instances follow (`StationCountLimit` -> `MaxStationsCost`,
  `MinimizeStations` -> `UsedStationsCost`, `WorkloadBalance` -> `WorkloadImbalanceCost`,
  every other class gains `Cost`). The model fields `same_station_groups` and
  `different_station_pairs` keep their names.
- **Raw costs are unchanged for every complete assignment** (each constraint's
  total, and the default score). Changed by design:
  - an unassigned task contributes 0 (0.1.5 scored `-1` as a station index: the
    unassigned web demo was `-18hard`, it is now `0hard`);
  - **exponent granularity**: the engine shapes a unit constraint per station and a
    global on its total. `max_stations_cost`, `task_station_bounds_cost`,
    `fixed_task_stations_cost` and `forbidden_task_stations_cost` are shaped per
    station (0.1.5: per task), `different_station_pairs_cost` per station (0.1.5: per
    pair) and `same_station_groups_cost` on its total (0.1.5: per group). Identical at
    exponent 1 (CP-SAT rejects any other exponent);
  - hard contributions are rounded per unit (0.1.5 rounded the hard total once):
    different only for fractional hard weights;
  - `constraint_costs()` returns the standard dict
    `{name: {"costs", "weight", "exponent", "hard", "global", "level", "reward",
    "level_flippable", "name", "label"}}` with per-station lists
    (`0 .. station_count - 1`; 0.1.5: `{name: [per station | task | group | pair]}`);
  - a constraint picked twice raises `ValueError` (0.1.5 counted it twice).
- **`SolverConfig`** is the engine's (the other C++ packages' knobs:
  `spent_limit_seconds`, `step_count_limit`, `unimproved_*`, `random_seed`,
  `parallel_solver_count`, `late_acceptance_size`, `full_assert`, `ruin_recreate*`,
  `change/swap_move_weight`, `value_recommender_probability`, `descent_weight`,
  `custom_move_probability`, `close_station_moves`, `best_score_limit`, `on_new_best`).
  The simulated-annealing knobs `max_iterations`, `stagnation_limit`, `temperature`,
  `cooling_rate`, the keyword constructor and the `anneal` / `configure` helpers are
  gone. Zero budget = `spent_limit_seconds = 0` (`step_count_limit = 0` means "no
  limit"); a bounded search = `step_count_limit = N`. `on_new_best(step, score)`
  (0.1.5: `on_new_best(state)`).
- `solve()` returns the engine `Score` (`.hard`, `.soft`; 0.1.5: a tuple) and takes the
  engine object or the model (written back in place). `calculate_score(problem,
  constraints)` returns the engine `Score` for the defaults and a
  `constraints.Score(hard, soft)` named tuple for a selection. `DEFAULTS` and the
  top-level `score` are gone (use `constraints.get_constraints()` /
  `constraints.score()`). With `constraints=...` a solve is single-start
  (`parallel_solver_count` is ignored), as in every C++ package.
- The JSON input/output schema of `AssemblyLineBalancingProblemModel` is unchanged.

## cpp/

- `include/alb/domain.hpp` — domain + `optimaru::SolutionDescriptor` specialization
- `include/alb/constraints/` — one component per constraint: `station_capacity_cost.hpp`,
  `task_precedence_cost.hpp`, `max_stations_cost.hpp`, `task_station_bounds_cost.hpp`,
  `fixed_task_stations_cost.hpp`, `forbidden_task_stations_cost.hpp`,
  `same_station_groups_cost.hpp`, `different_station_pairs_cost.hpp`,
  `used_stations_cost.hpp`, `workload_imbalance_cost.hpp` (+ `common.hpp`);
  `include/alb/constraints.hpp` assembles the default set (`defaultConstraints()`) and
  states the catalog order once (`forEachDefaultConstraint`)
- `construction` — the 0.1.5 construction, ported exactly (level-synchronous Kahn
  order, first fit on a station pointer that never moves down, fixed-station override)
- `score_calculator` — full recompute (the reference) and `rawCosts()`
- `incremental_calculator` — before/after hooks, O(degree) per move
- `solver` — engine wrapper (construction + Late Acceptance + adaptive ruin & recreate
  + `CloseStationMove` + the precedence-window value recommender)
- `src/main.cpp` — a 100-task demo against the trivial lower bound

```bash
cmake -S use-cases/assembly-line-balancing/cpp -B build/assembly-line-balancing -DCMAKE_BUILD_TYPE=Release
cmake --build build/assembly-line-balancing -j4
ctest --test-dir build/assembly-line-balancing --output-on-failure --no-tests=error
./build/assembly-line-balancing/alb_demo 5
```

`alb_test`: per-constraint hand checks (positive and zero), the construction cases,
incremental == full recompute over random change / swap / unassign / batch moves,
FULL_ASSERT solves, determinism and multi-start. `alb_plugin_test`: default assembly ==
monolith (score and every raw total), plugin incremental == reset, partial assemblies,
overrides, raw costs by hand per station, the catalog.

## Usage

```sh
cd use-cases/assembly-line-balancing/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import assembly_line_balancing as alb
from assembly_line_balancing.models import AssemblyLineBalancingProblemModel, PrecedenceModel, TaskModel

problem = AssemblyLineBalancingProblemModel(
    cycle_time=5,
    tasks=[TaskModel(id="A", duration=3), TaskModel(id="B", duration=3), TaskModel(id="C", duration=2)],
    precedences=[PrecedenceModel(before="A", after="B")],
)
alb.solve(problem, time_limit_seconds=1.0, random_seed=42)          # the model, in place
print({task.id: task.station for task in problem.tasks}, problem.stations_used)

core = problem.to_core()                                            # or the engine object
alb.solve(core, time_limit_seconds=1.0, random_seed=42)
print(AssemblyLineBalancingProblemModel.from_core(core).station_loads)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`,
`stop_at_first_feasible`, `on_new_best`, a full `config` (`alb.SolverConfig`) and
`constraints`:

```python
from assembly_line_balancing.constraints import WorkloadImbalanceCost, get_constraints, score

alb.constraint_info()          # [ConstraintInfo(name='station_capacity_cost', default_level='hard', ...), ...]
alb.solve(problem, time_limit_seconds=1.0, constraints=[*get_constraints()[:-1], WorkloadImbalanceCost(weight=1)])
raw = alb.constraint_costs(problem)                 # raw costs, re-priced in pure Python:
score(raw), score(raw, [*get_constraints()[:-1], WorkloadImbalanceCost(weight=2)])
```

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
from assembly_line_balancing import with_initial_solution

warm = with_initial_solution(problem, problem.model_dump(mode="json"))
alb.solve(warm, time_limit_seconds=1.0, random_seed=42)
print({task.id: task.station for task in warm.tasks}, warm.stations_used)
```

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-use-case).
The planning variable is **BASIC**; the domain solver that configures the engine is
`cpp/include/alb/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves (weights in
`Solver::Config`). `CloseStationMove` (empties a station by moving its tasks, in
topological order, into their precedence windows) is defined and registered in
`cpp/src/solver.cpp` (`runEngine`) — use it as the template. A new move is an
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)`; a ruin for
LNS is a `RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** `cpp/src/construction.cpp` places every task
before the engine starts (the engine only fills what is still unassigned). It is the
0.1.5 construction, ported exactly; `alb_test` and the Python reference-parity tests
pin its results.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/alb/constraints/`:
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint`
   per station — `PerTaskConstraint` for a per-task term — or a member-change
   `GlobalConstraint`; positive raw cost = penalty, 0 = no violation) declaring
   `static constexpr double kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
   an `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `cpp/include/alb/constraints.hpp` and add it to
   `forEachDefaultConstraint()` if it belongs to the default set (that is also the
   catalog order);
3. for a default constraint, add the same raw cost to `score_calculator` (`RawCosts`) /
   `incremental_calculator`, weighted by the component's `kDefaultWeight` —
   `alb_plugin_test` checks they agree bit for bit — and a positive and a zero
   hand-calculated case to `alb_test` / `alb_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `python/assembly_line_balancing/constraints.py`
   (plus its module-level default instance); callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRuleCost(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/assembly-line-balancing/cpsat`](../../benchmarks/assembly-line-balancing/cpsat)
   (a plugin subclassing the spec; defaults come from `constraint_info()`), with an exact
   encoding so `test_constraint_parity.py` can pin assignments.

## Benchmark

[`benchmarks/assembly-line-balancing`](../../benchmarks/assembly-line-balancing) compares this
use case with OR-Tools CP-SAT: generated lines (`assembly_line_balancing_cpsat.benchmark`) and
the public SALBP archive used by Hexaly (`bench/run_bench.py`, downloaded on demand and
checksum-verified; results in [`bench/RESULTS.md`](../../benchmarks/assembly-line-balancing/bench/RESULTS.md)).

```sh
cd benchmarks/assembly-line-balancing
uv sync --project cpsat --all-groups
uv run --project cpsat python -m bench.run_bench --dataset large --seconds 60
uv run --project cpsat python -m bench.run_bench --dataset very-large --exact-max-size 0 --seconds 60
```

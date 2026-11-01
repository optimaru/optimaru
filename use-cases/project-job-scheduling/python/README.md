# project_job — project job scheduling (multi-project RCPSP) Python package

The `project-job-scheduling` example model (Apache-2.0; see the repository NOTICE)
(multi-project / multi-mode RCPSP): pybind11 bindings of the `../cpp` implementation
ported onto the generic C++ engine in `include/optimaru/`.

## Install / test

```sh
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

## Usage

```python
import project_job as pj
from project_job.demo import generate_demo_data

schedule = generate_demo_data()  # pydantic model (reproduces the Java demo)
result = pj.solve(schedule, time_limit_seconds=10.0, random_seed=42)
print(result.score_hard, result.score_medium, result.score_soft)
for allocation in result.allocations:
    print(allocation.job_id, allocation.execution_mode_id, allocation.delay, allocation.start_date, allocation.end_date)
```

`solve` accepts a `_core.ProjectJobSchedule` (in place) or a
`models.ProjectJobScheduleModel` (the pydantic mirror for JSON serialization, id
references). `stop_at_first_feasible=True` stops as soon as hard 0 is reached.

Per-constraint raw costs (all four constraints are global: one total each), the
constraints' defaults and how to change them:

```python
from project_job.constraints import ProjectDelayCost, get_constraints, score

core = schedule.to_core()
pj.solve(core, time_limit_seconds=5.0, random_seed=42)
raw = pj.constraint_costs(core)
print({name: entry["costs"] for name, entry in raw.items()})
# {'non_renewable_resource_capacity_cost': [0.0], 'renewable_resource_capacity_cost': [0.0],
#  'project_delay_cost': [...], 'makespan_cost': [...]}
pj.constraint_info()
# [ConstraintInfo(name='non_renewable_resource_capacity_cost', default_level='hard',
#                 default_weight=1.0, level_flippable=True, reward=False, default_enabled=True), ...,
#  ConstraintInfo(name='project_delay_cost', default_level='medium', default_weight=1.0, ...), ...]
others = [spec for spec in get_constraints() if spec.name != "project_delay_cost"]
pj.solve(core, time_limit_seconds=5.0, constraints=[*others, ProjectDelayCost(hard=False)])  # delay as soft days
score(raw), score(raw, [*others, ProjectDelayCost(weight=2)])        # re-priced in pure Python
```

## Domain (as in the Java quickstart)

- **Project** — `release_date` + `critical_path_duration`. Used only as the reference date
  for delay (the critical path end); does not force the start.
- **Resource** — `renewable` (per-day capacity) / non-renewable (whole-horizon capacity),
  global (always renewable) or project-local.
- **Job** — SOURCE / STANDARD / SINK. One or more execution modes (duration + resource
  demand); SOURCE/SINK have one zero-duration mode.
- **Allocation** (planning entity) — 1:1 with jobs. Java's two planning variables
  `(executionMode, delay∈[0, 500))` are ported as one composite-encoded BASIC variable
  `mode_delay = delay * max_mode_count + mode_slot`
  (real mode index = `mode_slot % the job's mode count`, -1 == uninitialized).
  SOURCE/SINK allocations are always pinned (Java's `@PlanningPin` + `setPinned`).
- **Shadow chain** — Java's `predecessorsDoneDate → startDate → endDate` shadow variables
  are reproduced as derived state: recomputed in topological order, and the incremental
  calculator re-propagates only the successor subgraph of changed nodes.

## Constraints (HardMediumSoftScore, all penalties)

| Python name | C++ component | Default | Raw cost (0 = no violation) |
|---|---|---|---|
| `non_renewable_resource_capacity_cost` | `NonRenewableResourceCapacityCost` | hard, 1 | excess of total use over capacity, summed over the non-renewable resources |
| `renewable_resource_capacity_cost` | `RenewableResourceCapacityCost` | hard, 1 | excess of use over capacity, summed over renewable resource × day |
| `project_delay_cost` | `ProjectDelayCost` | medium, 1 | sum of SINK end date − critical path end date (positive only), in days |
| `makespan_cost` | `MakespanCost` | soft, 1 | latest SINK end date (a max over the projects) |

`constraint_info()` reports these defaults from the C++ components. The medium weight is in
medium units: `hard=True` on `project_delay_cost` costs 1 hard per day, `hard=False` 1 soft
per day, and an explicit weight stays on the medium level. Every spec also takes the
keyword-only `offset=0.0` / `clip=True`, which shift the shaping of the one total
(`clip=True` counts only what lies beyond the offset, `clip=False` shapes `x - offset` as it
is). A constraint may be picked several times (since 0.3.0); each pick is its own instance
with its own knobs, the instances add up, and `constraint_costs(schedule, picks)` keys the
k-th pick `<name>#<k>`: `score(raw, [MakespanCost(), MakespanCost(exponent=2, offset=40)])`.

0.2.0 renamed them (`non_renewable_resource_capacity`, `renewable_resource_capacity`,
`total_project_delay`, `total_makespan` before; classes `NonRenewableResourceCapacity`,
`RenewableResourceCapacity`, `TotalProjectDelay`, `TotalMakespan`), and the medium default
weight moved from 10^9 (soft) to 1 (medium units).

## Score mapping

Java uses `HardMediumSoftScore`; the engine only knows `HardSoftScore(int64 hard, double
soft)`. The same offset mapping as bed-allocation/meeting-scheduling is used, declared once
as the domain's medium fold (`kMediumFold` in `../cpp/include/pjs/domain.hpp`; every
`constraint_costs()` entry carries it as `medium_fold`, and
`project_job.constraints.score()` returns the folded engine-level `(hard, soft)`):

```
engine.hard = hard
engine.soft = medium * 10^9 + soft      # kMediumUnit = 10^9
```

medium (sum of delays) / soft (makespan) are integers and a double is exact up to 2^53, so
lexicographic order holds while `|medium| < ~9.0x10^6` and `|soft| < 10^9` (both bounded by
`allocations x (max delay + max duration)` — plenty of headroom on realistic
instances). Rule details are in `../cpp/include/pjs/domain.hpp`.

## Differences from the Java original (caveats)

- **Composite encoding**: two variables → one. Jobs with fewer modes than `max_mode_count`
  get duplicate encodings where several values decode to the same (mode, delay)
  (`slot % mode count`), but every value is valid, so Java's per-entity value range
  semantics are preserved. The demo (every STANDARD job has 3 modes) has no duplicates.
- **Initialization order / heuristic**: Java's `DelayStrengthComparator` (ascending delay
  strength) is reproduced equivalently by the engine CH scanning values upward from 0
  (no separate port).
- **Pinned uninitialized allocations**: the engine refuses an unassigned pinned entity,
  so `Solver.solve` first fixes uninitialized pinned allocations to (first mode, delay 0)
  (the SOURCE/SINK convention of the Java demo).
- **Allocation's sourceAllocation/sinkAllocation fields**: references unused by any Java
  constraint, so not ported (REST shape only).
- **Demo data**: scale and distributions follow the Java `DemoDataGenerator`, but the
  `java.util.Random(0)` stream itself is not reproduced.

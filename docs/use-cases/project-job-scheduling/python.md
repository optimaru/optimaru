# optimaru.project_job_scheduling — project job scheduling (multi-project RCPSP) Python package

Multi-project / multi-mode RCPSP, derived from the Timefold Quickstarts
`project-job-scheduling` example (Apache-2.0; see the repository NOTICE): pybind11 bindings of
the `optimaru/project_job_scheduling/cpp` implementation on the generic C++ engine in
`optimaru/cpp/optimaru/`.

## Install / test

```sh
OPTIMARU_USE_CASES=project_job_scheduling uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/project_job_scheduling
```

## Usage

```python
from optimaru import project_job_scheduling as pj
from optimaru.project_job_scheduling.demo import generate_demo_data

schedule = generate_demo_data()  # pydantic model (the demo instance)
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
from optimaru.project_job_scheduling.constraints import ProjectDelayCost, get_constraints, score

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
pj.solve(core, time_limit_seconds=5.0, constraints=[*others, ProjectDelayCost(level="soft")])  # delay as soft days
score(raw), score(raw, [*others, ProjectDelayCost(weight=2)])        # re-priced in pure Python
```

## Domain

- **Project** — `release_date` + `critical_path_duration`. Used only as the reference date
  for delay (the critical path end); does not force the start.
- **Resource** — `renewable` (per-day capacity) / non-renewable (whole-horizon capacity),
  global (always renewable) or project-local.
- **Job** — SOURCE / STANDARD / SINK. One or more execution modes (duration + resource
  demand); SOURCE/SINK have one zero-duration mode.
- **Allocation** (the decision) — 1:1 with jobs. The solver chooses an execution mode and a
  delay in `[0, delay_range_size)` (500 by default) per allocation:
  `AllocationModel.execution_mode_id` / `delay` (both `None` == uninitialized). In `_core`
  the pair is one composite-encoded assignment variable
  `mode_delay = delay * max_mode_count + mode_slot`
  (real mode index = `mode_slot % the job's mode count`, -1 == uninitialized).
  SOURCE/SINK allocations are always pinned; `pinned` also fixes a STANDARD one.
- **Derived dates** — `predecessors_done_date → start_date → end_date` are derived, not
  decided: an allocation starts `delay` days after its last predecessor ends and runs for
  its mode's duration. They are recomputed in topological order, and the incremental
  calculator re-propagates only the successor subgraph of changed nodes.

## Constraints (hard / medium / soft, all penalties)

| Python name | C++ component | Default | Raw cost (0 = no violation) |
|---|---|---|---|
| `non_renewable_resource_capacity_cost` | `NonRenewableResourceCapacityCost` | hard, 1 | excess of total use over capacity, summed over the non-renewable resources |
| `renewable_resource_capacity_cost` | `RenewableResourceCapacityCost` | hard, 1 | excess of use over capacity, summed over renewable resource × day |
| `project_delay_cost` | `ProjectDelayCost` | medium, 1 | sum of SINK end date − critical path end date (positive only), in days |
| `makespan_cost` | `MakespanCost` | soft, 1 | latest SINK end date (a max over the projects) |

`constraint_info()` reports these defaults from the C++ components. The medium weight is in
medium units: `level="hard"` on `project_delay_cost` costs 1 hard per day, `level="soft"` 1
soft per day, and an explicit weight is in units of the level in effect (medium by default).
Any constraint may take any of the three levels (`MakespanCost(level="medium")` charges one
medium unit per day). Every spec also takes the
keyword-only `offset=0.0` / `clip=True`, which shift the shaping of the one total
(`clip=True` counts only what lies beyond the offset, `clip=False` shapes `x - offset` as it
is, and is refused with an offset on hard, because hard 0 must mean no violation). A
constraint may be picked several times; each pick is its own instance
with its own knobs, its level included, the instances add up, and
`constraint_costs(schedule, picks)` keys the
k-th pick `<name>#<k>`: `score(raw, [MakespanCost(), MakespanCost(exponent=2, offset=40)])`.

## Score mapping

The score has three levels (hard / medium / soft); the engine only knows
`Score(int64 hard, double soft)`. The same offset mapping as bed-allocation/meeting-scheduling is used, declared once
as the domain's medium fold (`kMediumFold` in `optimaru/project_job_scheduling/cpp/include/pjs/domain.hpp`; every
`constraint_costs()` entry carries it as `medium_fold`, and
`optimaru.project_job_scheduling.constraints.score()` returns the folded engine-level `(hard, soft)`):

```
engine.hard = hard
engine.soft = medium * 10^9 + soft      # kMediumUnit = 10^9
```

medium (sum of delays) / soft (makespan) are integers and a double is exact up to 2^53, so
lexicographic order holds while `|medium| < ~9.0x10^6` and `|soft| < 10^9` (both bounded by
`allocations x (max delay + max duration)` — plenty of headroom on realistic
instances). Rule details are in `optimaru/project_job_scheduling/cpp/include/pjs/domain.hpp`.

## Caveats

- **Composite encoding**: the mode and the delay share one variable. Jobs with fewer modes
  than `max_mode_count` get duplicate encodings where several values decode to the same
  (mode, delay) (`slot % mode count`), but every value is valid for every job. The demo
  (every STANDARD job has 3 modes) has no duplicates.
- **Initialization order**: the construction heuristic scans values upward from 0, so it
  tries the smallest delays first.
- **Pinned uninitialized allocations**: the engine refuses an unassigned pinned entity,
  so `Solver.solve` first fixes uninitialized pinned allocations to (first mode, delay 0).
- **Demo data**: `generate_demo_data(seed)` draws the instance from a seeded
  `random.Random`.

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

| Constraint | Level | What it measures |
|---|---|---|
| nonRenewableResourceCapacity | hard | excess of total use over capacity per non-renewable resource |
| renewableResourceCapacity | hard | excess of use over capacity per renewable resource × day |
| totalProjectDelay | medium | sum of SINK end date − critical path end date (positive only) |
| totalMakespan | soft | latest SINK end date |

## Score mapping

Java uses `HardMediumSoftScore`; the engine only knows `HardSoftScore(int64 hard, double
soft)`. The same offset mapping as bed-allocation/meeting-scheduling is used:

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
- **Pinned uninitialized allocations**: the engine CH initializes every uninitialized
  entity without knowing about pinning, so `Solver.solve` first fixes uninitialized pinned
  allocations to (first mode, delay 0) (the SOURCE/SINK convention of the Java demo).
- **Allocation's sourceAllocation/sinkAllocation fields**: references unused by any Java
  constraint, so not ported (REST shape only).
- **Demo data**: scale and distributions follow the Java `DemoDataGenerator`, but the
  `java.util.Random(0)` stream itself is not reproduced.

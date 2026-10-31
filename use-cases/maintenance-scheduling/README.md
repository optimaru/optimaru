# Maintenance Scheduling (C++ / Python)

The `maintenance-scheduling` example model (Apache-2.0; see the repository NOTICE)
port: assigns a crew (Crew) and a start date to maintenance jobs (Job) to minimize early/late
maintenance. A C++ domain on top of the generic engine in `include/optimaru/` (late acceptance
+ ALNS), with a Python package (`maintenance_scheduling`) via pybind11.

## Domain encoding

The Java original's `Job` has two planning variables — `crew` and `startDate`
(a TimeGrain pattern excluding weekends) — plus the shadow variable `endDate`.
The engine supports one BASIC int variable per entity, so it follows the
composite encoding pattern of school-timetabling:

```
crewDate = startDateIndex * crewCount + crewIndex     # -1 == unassigned
```

- `startDateIndex` indexes the list of weekdays (Mon-Fri) of the calendar `[fromDate, toDate)`
  (`MaintenanceSchedule::startDates`, Java `createStartDateList()`).
- The shadow `endDate` is a pure function of `(duration, startDateIndex)`, so it is
  precomputed into a table (`endDateByDur`) in the prepare step without listeners. Like Java
  `Job.calculateEndDate`, it is the exclusive end date skipping weekends.
- All dates are epoch days (Java `LocalDate.toEpochDay`).

## Constraints (full port of the Java ConstraintProvider)

| Constraint | Level | Penalty (same semantics as Java) |
|---|---|---|
| crewConflict | Hard | -overlapping calendar days per overlapping job pair on the same crew |
| minStartDate | Hard | -1 per day started before minStartDate |
| maxEndDate | Hard | -1 per day ended after maxEndDate |
| beforeIdealEndDate | Soft | -1 per day ended before idealEndDate |
| afterIdealEndDate | Soft | -1,000,000 per day ended after idealEndDate |
| tagConflict | Soft | -1,000 per shared tag x overlapping day of overlapping job pairs |

Score mapping: the Java original already uses `HardSoftScore`, so it maps 1:1 onto the
engine's HardSoftScore(int64 hard, double soft), and the soft weights
(1 / 1,000 / 1,000,000) are the original ones. The two pairwise constraints reduce to sums
of C(n,2) over per-day occupancy counts (equivalent to summing pairwise overlapping days,
since the intervals are contiguous) and are scored incrementally in O(job calendar length)
per move.

An extension not in the original: `Job::pinned` (the engine's pinning hook) — counts in
the score, but the solver never moves it.

## Build / run

```sh
cd use-cases/maintenance-scheduling/cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
./build/mnt_test          # constraint hand checks + incremental==full + FULL_ASSERT
./build/mnt_demo          # reproduces the Java demo (SMALL), 10 s, checks hard 0
./build/mnt_demo 15 --large

cd ../python
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

The demo data reproduces the Java `DemoDataGenerator` bit for bit with a
`java.util.Random(17)` replica (SMALL: 3 crews/8 weeks/14 jobs, LARGE: 5 crews/16 weeks/
48 jobs; starting next Monday). Both SMALL and LARGE reach hard 0 within seconds.

## Usage

```sh
cd use-cases/maintenance-scheduling/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import maintenance_scheduling as mnt

schedule = mnt.generate_demo_data()
result = mnt.solve(schedule, time_limit_seconds=5.0, random_seed=42)
print(result.score)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`maintenance_scheduling.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/maintenance-scheduling/cpp -B build/maintenance-scheduling && cmake --build build/maintenance-scheduling -j
ctest --test-dir build/maintenance-scheduling
```

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose search starts from `solution`, which is `MaintenanceScheduleModel.from_core(schedule)` or that model as a JSON dict. `solve` takes the engine object, so convert the result with `to_core()`:

```python
from maintenance_scheduling.models import MaintenanceScheduleModel

problem = MaintenanceScheduleModel.from_core(mnt.generate_demo_data())
solved = MaintenanceScheduleModel.from_core(schedule)           # `schedule` solved above
restart = mnt.with_initial_solution(problem, solved).to_core()
mnt.solve(restart, time_limit_seconds=5.0, random_seed=43)
print(restart.score)
```

Jobs are matched by id. A job the solution leaves out is placed by the construction heuristic, and a complete solution is exactly where the search starts. Pinned jobs keep their crew and start date on `problem`, whatever the solution says. `ValueError` is raised for an unknown id, for a job with only one of `crew_id` and `start_date`, and for a start date that is not a weekday inside the work calendar.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/mnt/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`). This use case relies on the engine's generic moves only; it has no domain move yet. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` next
to the engine construction in `cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** Entities are placed in index order (First Fit). Set
`engineConfig.constructionHeuristic.entityDifficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/mnt/constraints/` (currently `after_ideal_end_date`, `before_ideal_end_date`, `crew_conflict`, `max_end_date`, `min_start_date`, `tag_conflict`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/mnt/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `mnt_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/maintenance_scheduling/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/maintenance-scheduling/cpsat`](../../benchmarks/maintenance-scheduling/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/maintenance-scheduling`](../../benchmarks/maintenance-scheduling) compares this use case with OR-Tools CP-SAT on the same instances.

# Maintenance Scheduling (C++ / Python)

The Timefold Quickstarts `maintenance-scheduling` example model (Apache-2.0; see the repository [NOTICE](../../NOTICE))
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

## cpp/

- `include/mnt/domain.hpp` — domain + `optimaru::SolutionDescriptor` specialization
- `include/mnt/constraints/` — one component per constraint: `min_start_date_cost.hpp`,
  `max_end_date_cost.hpp`, `before_ideal_end_date_cost.hpp`, `after_ideal_end_date_cost.hpp`,
  `crew_conflict_cost.hpp`, `tag_conflict_cost.hpp` (+ `common.hpp`: the per-job and
  day-overlap bases); `include/mnt/constraints.hpp` assembles the default set
  (`defaultConstraints()`)
- `score_calculator` — full recompute (the reference)
- `incremental_calculator` — before/after hooks, O(job calendar length) per move
- `solver` — engine wrapper (CH + Late Acceptance + ALNS, guided selection)

## Constraints (full port of the Java ConstraintProvider)

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `min_start_date_cost` | `MinStartDateCost` | hard | 1 | per (start date, crew) cell: days its jobs start before their `min_start_date` |
| `max_end_date_cost` | `MaxEndDateCost` | hard | 1 | per cell: days its jobs' (exclusive) end date passes their `max_end_date` |
| `before_ideal_end_date_cost` | `BeforeIdealEndDateCost` | soft | 1 | per cell: days its jobs end before their `ideal_end_date` |
| `after_ideal_end_date_cost` | `AfterIdealEndDateCost` | soft | 1,000,000 | per cell: days its jobs end after their `ideal_end_date` |
| `crew_conflict_cost` | `CrewConflictCost` | hard | 1 | global total: overlapping calendar days summed over same-crew job pairs |
| `tag_conflict_cost` | `TagConflictCost` | soft | 1,000 | global total: shared tags x overlapping days summed over job pairs |

The rows are in `list_constraints()` order (the four per-job unit constraints first, the two
global ones after them). Every constraint is flippable (`level="hard"` / `level="soft"`, or
`Level.HARD` / `Level.SOFT`; `level="medium"` raises `ValueError`, the domain has no medium
level), none is a reward and all six are scored by default (`default_enabled`);
`maintenance_scheduling.constraint_info()` reports exactly this table from the C++
components (`kDefaultLevel` / `kDefaultWeight`), and the CP-SAT benchmark reads the same
rows. `exponent` shapes the date rules per cell and the two global conflict rules on their
single total. Unassigned jobs contribute nothing.

**0.2.0:** the constraints were renamed with a `_cost` suffix (`min_start_date` ->
`min_start_date_cost`, `max_end_date` -> `max_end_date_cost`, `before_ideal_end_date` ->
`before_ideal_end_date_cost`, `after_ideal_end_date` -> `after_ideal_end_date_cost`,
`crew_conflict` -> `crew_conflict_cost`, `tag_conflict` -> `tag_conflict_cost`; spec
classes, default instances and C++ structs likewise, e.g. `CrewConflict` ->
`CrewConflictCost`, registry `"crewConflict"` -> `"crewConflictCost"`). The suffix also
separates the constraint names from the job fields `min_start_date` / `max_end_date`, which
keep their names. The old names are not accepted any more (no aliases). The defaults are
declared once, in the C++ components; the CP-SAT plugins no longer restate the 1,000,000 /
1,000 weights. Raw costs and the default score are unchanged.

**0.3.0:** every constraint takes two more knobs, `offset` and `clip` (keyword-only), and
may be picked several times. A raw cost `x` (per cell for the four date rules, the single
total for the two conflict rules — the granularity of `exponent`) is shaped as
`sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which lowers the penalty of
every cell below the offset, an empty one included: every crew on every start date);
`offset=0` is the 0.2.0 shaping. Both apply per cell (or to a conflict rule's one total),
not to the sum over the cells. `clip=False` with an offset is refused on hard (the two
start/end date rules and `crew_conflict_cost` by default), because hard 0 must mean no
violation. Each pick is its own instance with its own knobs, its level included (`level`
replaced the `hard` knob; `hard=` raises `TypeError`), and the instances add up:

```python
from maintenance_scheduling.constraints import TagConflictCost, get_constraints

mnt.solve(schedule, constraints=[
    *get_constraints(),                                  # the defaults
    TagConflictCost(weight=100, exponent=2, offset=10),  # + 100 x (tag-conflict days beyond 10)^2
])
```

`mnt.constraint_costs(schedule, picks)` keys the second pick `tag_conflict_cost#2`. The
CP-SAT reference takes integral offsets.

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
./build/mnt_plugin_test   # plugin assembly == monolith, raw costs by hand, catalog, overrides
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

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-use-case).
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
`cpp/include/mnt/constraints/` (currently `min_start_date_cost`, `max_end_date_cost`,
`before_ideal_end_date_cost`, `after_ideal_end_date_cost`, `crew_conflict_cost`,
`tag_conflict_cost`):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` —
   usually a `PerJobConstraint` — or a `GlobalConstraint` such as a `DayOverlapGlobalConstraint`;
   positive raw cost = penalty, 0 = no violation) declaring
   `static constexpr double kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
   an `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `cpp/include/mnt/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator`, weighted by the component's
   `kDefaultWeight` — `mnt_plugin_test` checks they agree bit for bit — and a positive and a
   zero hand-calculated case to `mnt_test` / `mnt_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `python/maintenance_scheduling/constraints.py`
   (plus its module-level default instance); callers then pick, reweight or flip it:
   `constraints=[..., MyRuleCost(weight=5, level="hard")]`;
5. mirror it in the CP-SAT model in
   [`benchmarks/maintenance-scheduling/cpsat`](../../benchmarks/maintenance-scheduling/cpsat) (a
   plugin subclassing the spec; defaults come from `constraint_info()`) so the benchmark stays
   like for like.

## Benchmark

[`benchmarks/maintenance-scheduling`](../../benchmarks/maintenance-scheduling) compares this use case with OR-Tools CP-SAT on the same instances.

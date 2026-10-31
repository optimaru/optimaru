# Food Packaging (C++ / Python)

A C++ engine port of the The `food-packaging` example model (Apache-2.0; see the repository NOTICE).
Assigns packaging jobs (Job) to production lines (Line) **in order** — every product change
inserts a cleanup time that depends on the previous product, and the cleaning windows of
lines run by the same operator must not overlap.

- `cpp/` — domain + constraints (full/incremental) + solver wrapper + demo/tests.
  Uses the engine in `include/` (header-only) as-is. `cpp/include/fp/constraints/` holds
  one component per constraint (`max_end_time_cost.hpp`, `after_ideal_end_time_cost.hpp`,
  `squared_makespan_cost.hpp`, `operator_cleaning_conflict_cost.hpp`,
  `unassigned_jobs_cost.hpp`, plus `common.hpp`); `cpp/include/fp/constraints.hpp`
  assembles the default set (`defaultConstraints()`).
- `python/` — pybind11 bindings (`food_packaging._core`) and the pydantic v2 mirror.

## Model summary

| Java | Port |
|---|---|
| `Line.jobs` `@PlanningListVariable(allowsUnassignedValues=true)` | engine LIST variable. Unassigned is an unassigned pool exposed to the engine as one more list |
| `Line.operator` `@PlanningVariable` | reassigned by the solver wrapper with hill climbing between engine legs (alternating optimization) |
| `Job` shadow chain (`startCleaning/startProduction/end`) | derived from list positions, O(length) incremental recompute per line |
| `@PlanningPin` | structural pinning that hides the line's pinned prefix from the engine view |
| `HardMediumSoftScore` | `engine.soft = medium * 10^10 + soft` mapping, the domain's medium fold (see the domain header comments) |

## Constraints (HardMediumSoftScore, folded into the engine's HardSoftScore)

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `max_end_time_cost` | `MaxEndTimeCost` | hard | 1 | per line: minutes its jobs end after `max_end_time` |
| `after_ideal_end_time_cost` | `AfterIdealEndTimeCost` | medium | 1 | per line: minutes its jobs end after `ideal_end_time` |
| `squared_makespan_cost` | `SquaredMakespanCost` | soft | 1 | per line: (end of its last job - line start)^2, 0 for an empty line |
| `operator_cleaning_conflict_cost` | `OperatorCleaningConflictCost` | hard | 1 | global: overlapping cleaning minutes of job pairs on lines of one operator |
| `unassigned_jobs_cost` | `UnassignedJobsCost` | medium | 1 | global: production minutes of the jobs on no line |

`list_constraints()` and `constraint_info()` report them in this order (the unit
constraints, then the two globals); `food_packaging.constraint_info()` reports exactly
this table from the C++ components (`kDefaultLevel` / `kDefaultWeight`). Every
constraint is flippable (`hard=True` / `hard=False`), none is a reward and all five
are scored by default (`default_enabled`).

The medium weights are in **medium units**: the domain folds the medium level into the
engine's soft level as `engine.soft = medium * 10^10 + soft` (`kMediumFold` in
`cpp/include/fp/domain.hpp`, reported as `medium_fold: {"kind": "into_soft", "unit": 1e10}`
on every `constraint_costs()` entry). Medium is a default-only level: `hard=True` makes a
medium constraint 1 hard unit per minute, `hard=False` 1 soft unit per minute (outside the
fold), and an explicit `weight` keeps it on the medium level, in medium units — in the
engine and in the CP-SAT reference alike.

**0.2.0:** the constraints were renamed (`max_end_date_time` -> `max_end_time_cost`,
`operator_cleaning_conflict` -> `operator_cleaning_conflict_cost`, `ideal_end_date_time`
-> `after_ideal_end_time_cost`, `maximize_jobs_assigned` -> `unassigned_jobs_cost`,
`minimize_makespan` -> `squared_makespan_cost`; the spec classes, module-level default
instances, C++ structs, headers and registry names follow). The two medium constraints
used to carry the fold as a soft weight of 10^10 (some docs said 10^9, which was never
the value); now their default weight is 1 on the medium level and the fold belongs to the
domain, so `hard=True` costs 1 hard per minute (was 10^10) and `hard=False` demotes them
to soft in both solvers (it was a no-op in the engine and landed on medium in CP-SAT).
`analyze()` / `ConstraintBreakdown` are no longer exposed to Python:
`constraint_costs()` is the one per-constraint surface. Raw costs and the default score
are unchanged.

## Build/run

```sh
cd cpp && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
./build/fp_test          # constraint hand checks + incremental==full + FULL_ASSERT + demo feasible
./build/fp_plugin_test   # plugin assembly == monolith, raw costs by hand, catalog, medium knobs
./build/fp_demo [sec]    # reproduces the Java demo (5 lines, 100 jobs), default 10 s

cd ../python && uv venv && uv pip install -e . pytest pydantic && uv run pytest -q
```

## Usage

```sh
cd use-cases/food-packaging/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import food_packaging as fp

solution = fp.generate_demo_data()
result = fp.solve(solution, time_limit_seconds=5.0, random_seed=42)
print(result.score)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`food_packaging.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/food-packaging/cpp -B build/food-packaging && cmake --build build/food-packaging -j
ctest --test-dir build/food-packaging
```

## Starting from an existing solution

```python
from food_packaging.models import PackagingScheduleModel

problem = PackagingScheduleModel.from_core(fp.generate_demo_data())
solved = PackagingScheduleModel.from_core(fp.solve(problem.to_core(), time_limit_seconds=2.0, random_seed=42))
seeded = fp.with_initial_solution(problem, solved)  # or solved.model_dump(mode="json")
better = fp.solve(seeded.to_core(), time_limit_seconds=2.0, random_seed=42)
```

`with_initial_solution` returns a new problem whose lines carry the solution's `job_ids` and `operator_id`, replacing the ones the problem had. The solve keeps those lists as its starting point and construction places only the jobs in no line, so a partial solution works. Pinned jobs stay as the problem has them: a line in the solution must start with its pinned jobs. An unknown id, a job given twice, a moved pinned job or two neighbouring jobs with no cleaning time between their products raise `ValueError`.

Two things differ from the other use cases, on the default solve only (without `constraints=`):

- Seeded operators can change. `improve_operator_assignment` runs after the search even with no time to search, and reassigns an operator whenever that strictly cuts the cleaning conflict.
- A time limit of 1 ms or less returns before construction, so left-out jobs stay unassigned. A complete solution comes back as it went in, but to have a partial one filled give it a longer limit.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **LIST**; the domain solver that configures the
engine is `cpp/include/fp/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes relocate / swap / 2-opt / Or-opt / 2-opt\* tail swap moves
(weights in `Solver::Config`). This use case relies on the engine's generic moves only; it has no domain move yet. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` next
to the engine construction in `cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** Elements are placed by cheapest insertion in index order. Set
`engineConfig.constructionHeuristic.entityDifficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/fp/constraints/` (currently `max_end_time_cost`, `after_ideal_end_time_cost`,
`squared_makespan_cost`, `operator_cleaning_conflict_cost`, `unassigned_jobs_cost`):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint`; positive raw cost = penalty, 0 = no violation) declaring
   `static constexpr double kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
   an `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter); a medium
   constraint declares `Level::Medium` at a weight in medium units (the domain applies the
   10^10 fold);
2. include it in `cpp/include/fp/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator`, weighted by the component's
   `kDefaultWeight` — `fp_plugin_test` checks they agree bit for bit — and a positive and a
   zero hand-calculated case to `fp_test` / `fp_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `python/food_packaging/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRuleCost(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/food-packaging/cpsat`](../../benchmarks/food-packaging/cpsat) (a plugin
   subclassing the spec; defaults come from `constraint_info()`) so the benchmark stays like
   for like.

## Benchmark

[`benchmarks/food-packaging`](../../benchmarks/food-packaging) compares this use case with OR-Tools CP-SAT on the same instances.

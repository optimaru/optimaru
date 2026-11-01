# Project Job Scheduling

Domain model, constraints and API details: [`python/`](python/README.md).

## Usage

```sh
cd use-cases/project-job-scheduling/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import project_job as pj
from project_job.demo import generate_demo_data

schedule = generate_demo_data()
result = pj.solve(schedule, time_limit_seconds=5.0, random_seed=42)
print(result.score_hard, result.score_medium, result.score_soft)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`project_job.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/project-job-scheduling/cpp -B build/project-job-scheduling && cmake --build build/project-job-scheduling -j
ctest --test-dir build/project-job-scheduling
```

`pjs_test` hand-checks the monolithic score (the Java provider-test cases), incremental ==
full recompute and a FULL_ASSERT solve; `pjs_plugin_test` checks the default assembly ==
the monolith, the raw costs by hand (one positive and one zero case per constraint), the
catalog and the medium knobs.

## Constraints (HardMediumSoftScore, folded into the engine's HardSoftScore)

`cpp/include/pjs/constraints/` holds one component per constraint
(`non_renewable_resource_capacity_cost.hpp`, `renewable_resource_capacity_cost.hpp`,
`project_delay_cost.hpp`, `makespan_cost.hpp`, plus `common.hpp`);
`cpp/include/pjs/constraints.hpp` assembles the default set (`defaultConstraints()`).
All four are global constraints (each reads the whole precedence chain or the whole
stock), so each has one raw total, and the exponent shapes that total.

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `non_renewable_resource_capacity_cost` | `NonRenewableResourceCapacityCost` | hard | 1 | resource units of total use above capacity, summed over the non-renewable resources |
| `renewable_resource_capacity_cost` | `RenewableResourceCapacityCost` | hard | 1 | resource-unit-days of use above capacity, summed over (renewable resource, day) |
| `project_delay_cost` | `ProjectDelayCost` | medium | 1 | days the SINK of each project ends after its critical-path end date (release + critical path duration), summed over the projects |
| `makespan_cost` | `MakespanCost` | soft | 1 | the latest SINK end date, an absolute day index (one value for the schedule, a max, not a sum) |

`list_constraints()` and `constraint_info()` report them in this order;
`project_job.constraint_info()` reports exactly this table from the C++ components
(`kDefaultLevel` / `kDefaultWeight`). Every constraint is flippable (`level="hard"` /
`level="soft"`), none is a reward and all four are scored by default (`default_enabled`).

The medium weight is in **medium units**: the domain folds the medium level into the
engine's soft level as `engine.soft = medium * 10^9 + soft` (`kMediumFold` in
`cpp/include/pjs/domain.hpp`, reported as `medium_fold: {"kind": "into_soft", "unit": 1e9}`
on every `constraint_costs()` entry). `level="hard"` makes
`project_delay_cost` 1 hard unit per day, `level="soft"` 1 soft unit per day (outside the
fold), and a `weight` is in units of the level in effect — in the
engine and in the CP-SAT reference alike.

**0.2.0:** the constraints were renamed (`non_renewable_resource_capacity` ->
`non_renewable_resource_capacity_cost`, `renewable_resource_capacity` ->
`renewable_resource_capacity_cost`, `total_project_delay` -> `project_delay_cost`,
`total_makespan` -> `makespan_cost`; the spec classes, module-level default instances, C++
structs, headers and registry names follow: `TotalProjectDelay` -> `ProjectDelayCost`,
`TotalMakespan` -> `MakespanCost`, ...). `makespan_cost` drops "total" because it is the
latest end date (a max), not a total. `project_delay_cost` used to carry the fold as a soft
weight of 10^9; now its default weight is 1 on the medium level and the fold belongs to the
domain, so `level="hard"` costs 1 hard per day (was 10^9), `level="soft"` demotes it to soft in
both solvers (it was a no-op in the engine), an explicit weight is in medium units (it used
to replace the fold), and on CP-SAT `level="soft"` on a hard constraint now lands on soft (it
landed on medium). Raw costs and the default score are unchanged.

**0.3.0:** every constraint takes two more knobs, `offset` and `clip` (keyword-only), and
may be picked several times. All four constraints are global, so each instance shapes its
one total `x` once: `sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default)
or `(x - offset) ** exponent`, sign-preserving (`clip=False`, which acts as a reward below
the offset, e.g. a makespan under a target day); `offset=0` is the 0.2.0 shaping. Each pick
is its own instance with its own knobs, and the instances add up:

```python
pj.solve(core, constraints=[
    *get_constraints(),
    MakespanCost(exponent=2, offset=40),   # + (days beyond day 40)^2
])
```

`pj.constraint_costs(core, picks)` keys the second pick `makespan_cost#2`. The domain
`Score` keeps integer levels, so fractional weights round in `schedule.score` (as before);
`constraints.score()` reports the exact engine value. The CP-SAT reference takes integral
offsets on every constraint (one term per global total).

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose search starts from `solution`, which is the model a solve returned or that model as a JSON dict:

```python
solved = pj.solve(schedule, time_limit_seconds=5.0, random_seed=42)
seeded = pj.with_initial_solution(schedule, solved.model_dump(mode="json"))
result = pj.solve(seeded, time_limit_seconds=5.0, random_seed=43)
print(result.score_hard, result.score_medium, result.score_soft)
```

Allocations are matched by id. An allocation the solution leaves out is placed by the construction heuristic, and a complete solution is exactly where the search starts. Pinned allocations, which always include SOURCE and SINK, keep their value on `problem`, whatever the solution says. `ValueError` is raised for an unknown id, for a mode that is not one of the job's, for a delay without a mode and for a delay outside the delay range, before anything reaches the engine, which does not range-check.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/pjs/solver.hpp` + `cpp/src/solver.cpp`.

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
`cpp/include/pjs/constraints/` (currently `non_renewable_resource_capacity_cost`,
`renewable_resource_capacity_cost`, `project_delay_cost`, `makespan_cost`):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint`, here usually over `ScheduleAggregateConstraint`; positive raw cost =
   penalty, 0 = no violation) declaring `static constexpr double kDefaultWeight` and
   `static constexpr optimaru::Level kDefaultLevel`, an
   `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter); a medium
   constraint declares `Level::Medium` at a weight in medium units (the domain applies the
   10^9 fold);
2. include it in `cpp/include/pjs/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator`, weighted by the component's
   `kDefaultWeight` — `pjs_plugin_test` checks they agree bit for bit — and a positive and a
   zero hand-calculated case to `pjs_test` / `pjs_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `python/project_job/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRuleCost(weight=5, level="hard")])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/project-job-scheduling/cpsat`](../../benchmarks/project-job-scheduling/cpsat)
   (a plugin subclassing the spec; defaults come from `constraint_info()`) so the benchmark
   stays like for like.

## Benchmark

[`benchmarks/project-job-scheduling`](../../benchmarks/project-job-scheduling) compares this use case with OR-Tools CP-SAT on the same instances.

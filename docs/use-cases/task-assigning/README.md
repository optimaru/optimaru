# Task Assigning

Derived from the Timefold Quickstarts `task-assigning` example (Apache-2.0), translated
and modified; see the repository [NOTICE](../../../NOTICE).

Domain model, constraints and API details: [`python.md`](python.md).

## Usage

```sh
OPTIMARU_USE_CASES=task_assigning uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/task_assigning
```

```python
from optimaru import task_assigning as ta
from optimaru.task_assigning.demo import generate_demo_data

solution = generate_demo_data()
result = ta.solve(solution, time_limit_seconds=5.0, random_seed=42)
print(result.score_hard, result.score_soft0, result.score_soft1, result.score_soft2)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`, `full_assert`, `on_new_best`, a full `config` (`task_assigning.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -B build -DOPTIMARU_USE_CASES=task_assigning && cmake --build build -j
ctest --test-dir build
```

## Constraints

The score has four levels, [1]hard/[3]soft, on the engine's hard / medium / soft levels.

| Constraint (Python name) | C++ component | Score level | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|---|
| `required_skill_cost` | `RequiredSkillCost` | hard[0] | hard | 1 | per employee: required skills of its tasks' types it lacks |
| `squared_makespan_cost` | `SquaredMakespanCost` | soft[1] | soft | 10^7 | per employee: (end of its last task)^2, minutes^2 |
| `critical_priority_task_end_time_cost` | `CriticalPriorityTaskEndTimeCost` | soft[2] | soft | 4 | per employee: summed end times of its CRITICAL tasks (min) |
| `major_priority_task_end_time_cost` | `MajorPriorityTaskEndTimeCost` | soft[2] | soft | 2 | per employee: summed end times of its MAJOR tasks (min) |
| `minor_priority_task_end_time_cost` | `MinorPriorityTaskEndTimeCost` | soft[2] | soft | 1 | per employee: summed end times of its MINOR tasks (min) |
| `unassigned_tasks_cost` | `UnassignedTasksCost` | soft[0] | **medium** | 1 | global: tasks on no employee's list |

This is the `list_constraints()` order (the unit constraints first, the global
`unassigned_tasks_cost` last). Every constraint is flippable onto any of the three levels
(`level="hard"`, `"medium"` or `"soft"`, or a `Level`), none is a reward and all six are
scored by default (`default_enabled`);
`task_assigning.constraint_info()` reports exactly this table from the C++ components
(`kDefaultLevel` / `kDefaultWeight`).

**Score mapping.** The engine scores three levels and this domain folds them into its
`Score` (`optimaru/task_assigning/cpp/include/ta/domain.hpp`):

```
engine.hard = hard * 10^9 + medium     # medium = soft0: MediumFold::belowHard(10^9)
engine.soft = soft                     # = soft1 * 10^7 + soft2 at the default weights
```

The medium level (`unassigned_tasks_cost`, soft[0]) folds **below** hard, so one
missing skill outweighs any number of unassigned tasks below 10^9. The soft[1]
and soft[2] share the engine's soft level; `squared_makespan_cost`'s default weight 10^7
is what keeps soft[1] above soft[2] (the `Score` adapter decodes `soft1 = soft / 10^7`,
`soft2 = soft % 10^7`). Every weight is in units of its level: `RequiredSkillCost(weight=3)`
is 3 hard units (3 * 10^9 engine units) per missing skill, `UnassignedTasksCost(weight=3)` 3
medium units per task. `UnassignedTasksCost(level="hard")` is
one hard unit per unassigned task, `UnassignedTasksCost(level="soft")` one soft unit per
unassigned task (outside the fold), in the engine and in CP-SAT alike. Any other constraint
may move onto medium the same way: `SquaredMakespanCost(level="medium", weight=1)` charges
one medium unit per squared minute, counted in the engine's hard level below the missing
skills and rounded per employee like a hard term, and a repeated pick carries its own level.
A selection without the medium constraint still folds the hard level by 10^9.

`squared_makespan_cost` is the sum of squared end times, not the makespan max.
`unassigned_tasks_cost` is a **medium** constraint and `required_skill_cost`'s default
weight is **1**: the 10^9 between a missing skill and an unassigned task is the below-hard
medium fold, not part of a weight. An explicit weight on `required_skill_cost`, or on a
constraint made hard with `level="hard"`, is in hard units (one hard unit = 10^9 engine
units: `RequiredSkillCost(weight=1)` is 10^9 engine units per missing skill, and
`CriticalPriorityTaskEndTimeCost(level="hard")` lands in the hard level);
`RequiredSkillCost(level="soft")` is 1 soft unit per missing skill, and
`UnassignedTasksCost(level="soft")` one soft unit per task. CP-SAT scores on the same three
levels (`("hard", "medium", "soft")`): the soft[1]-over-soft[2] order is carried by
`squared_makespan_cost`'s default weight 10^7 on the one soft level, as in the engine, and
a pick moved to soft lands on the soft level there too. The folded CP-SAT objective does
not fit one int64 for the demo, so CP-SAT optimizes the three levels one after another
(cpsat-common's staged solve).

Every constraint takes the knobs `weight`, `level`, `exponent`, `offset` and `clip`
(`offset` and `clip` keyword-only), and may be picked several times. A raw cost `x` (per employee for the five unit constraints, on
the one total for `unassigned_tasks_cost`) is shaped as `sign(x) * max(0, |x| - offset) **
exponent` (`clip=True`, the default) or `(x - offset) ** exponent`, sign-preserving
(`clip=False`, which charges every employee, an idle one included, and acts as a reward
below the offset); `offset=0` is the plain power. Both apply per employee (or on the
global's total), not to the total violation. `clip=False` with an offset is refused on hard
(`required_skill_cost` by default) and on medium (`unassigned_tasks_cost` by default),
because medium folds below hard here, so it counts toward the hard score, and hard 0 must
mean no violation; use a threshold (`clip=True`) there, or move the pick to soft. Each pick
is its own instance with its own knobs, and the instances add up:

```python
ta.solve(solution, constraints=[
    *get_constraints(),
    CriticalPriorityTaskEndTimeCost(exponent=2, offset=120),  # + (critical end times beyond 120)^2
])
```

`ta.constraint_costs(solution.to_core(), picks)` (it takes the engine object, not the
pydantic model) keys the second pick
`critical_priority_task_end_time_cost#2`. The CP-SAT reference takes integral offsets on
every constraint (one term per employee, one for the global total).

## Starting from an existing solution

```python
problem = generate_demo_data()
solved = ta.solve(problem, time_limit_seconds=2.0, random_seed=42)
seeded = ta.with_initial_solution(problem, solved)  # or solved.model_dump(mode="json")
better = ta.solve(seeded, time_limit_seconds=2.0, random_seed=42)
```

`with_initial_solution` returns a new problem whose employees carry the solution's `tasks`, replacing the ones the problem had. The solve keeps those lists as its starting point and construction places only the tasks in no list, so a partial solution works. With `time_limit_seconds=0` a complete solution comes back as it went in. An unknown id or a task assigned twice raises `ValueError`.

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **sequence** shape; the domain solver that configures the
engine is `optimaru/task_assigning/cpp/include/ta/solver.hpp` + `optimaru/task_assigning/cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes relocate / swap / 2-opt / Or-opt / 2-opt\* tail swap moves
(weights in `Solver::Config`). This use case relies on the engine's generic moves only; it has no domain move yet. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.add_move_operator(...)` next
to the engine construction in `optimaru/task_assigning/cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** Elements are placed by cheapest insertion in index order. Set
`engineConfig.construction.difficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`optimaru/task_assigning/cpp/include/ta/constraints/` (currently `required_skill_cost`, `squared_makespan_cost`,
`critical_priority_task_end_time_cost`, `major_priority_task_end_time_cost`,
`minor_priority_task_end_time_cost`, `unassigned_tasks_cost`; `common.hpp` holds the chain
walk and the priority-family base):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint`; positive raw cost = penalty, 0 = no violation) declaring
   `static constexpr double kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
   an `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `optimaru/task_assigning/cpp/include/ta/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator`, weighted by the component's
   `kDefaultWeight` — `ta_plugin_test` checks they agree bit for bit — and a positive and a
   zero hand-calculated case to `ta_test` / `ta_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `optimaru/task_assigning/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRuleCost(weight=5, level="hard")])`;
5. mirror it in the CP-SAT model in external-solver's
   [`external_solver.task_assigning.constraints`](https://github.com/optimaru/external-solver/blob/v0.1.0/src/external_solver/task_assigning/constraints.py) (a plugin
   subclassing the spec; defaults come from `constraint_info()`) so the benchmark stays like
   for like.

## Benchmark

[`benchmarks/task-assigning`](../../../benchmarks/task-assigning) compares this use case with OR-Tools CP-SAT on the same instances.

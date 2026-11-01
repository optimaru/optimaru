# Task Assigning

Domain model, constraints and API details: [`python/`](python/README.md).

## Usage

```sh
cd use-cases/task-assigning/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import task_assigning as ta
from task_assigning.demo import generate_demo_data

solution = generate_demo_data()
result = ta.solve(solution, time_limit_seconds=5.0, random_seed=42)
print(result.score_hard, result.score_soft0, result.score_soft1, result.score_soft2)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`task_assigning.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/task-assigning/cpp -B build/task-assigning && cmake --build build/task-assigning -j
ctest --test-dir build/task-assigning
```

## Constraints (BendableScore [1]hard/[3]soft on the engine's hard / medium / soft levels)

| Constraint (Python name) | C++ component | Bendable level | Default level | Default weight | Raw cost (0 = no violation) |
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
`HardSoftScore` (`cpp/include/ta/domain.hpp`):

```
engine.hard = hard * 10^9 + medium     # medium = soft0: MediumFold::belowHard(10^9)
engine.soft = soft                     # = soft1 * 10^7 + soft2 at the default weights
```

The medium level (`unassigned_tasks_cost`, Bendable soft[0]) folds **below** hard, so one
missing skill outweighs any number of unassigned tasks below 10^9. The Bendable soft[1]
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

**0.2.0:** the constraints were renamed — `no_missing_skills` -> `required_skill_cost`,
`minimize_unassigned_tasks` -> `unassigned_tasks_cost`, `minimize_makespan` ->
`squared_makespan_cost` (it is the sum of squared end times, not the makespan max),
`{critical,major,minor}_priority_task_end_time` -> `..._cost`; the classes
(`NoMissingSkills` -> `RequiredSkillCost`, `MinimizeUnassignedTasks` -> `UnassignedTasksCost`,
`MinimizeMakespan` -> `SquaredMakespanCost`, `*PriorityTaskEndTime` ->
`*PriorityTaskEndTimeCost`), the C++ components and registry names
(`noMissingSkills` -> `requiredSkillCost`, ...) and the module-level default instances follow
(no aliases). `unassigned_tasks_cost` is now a **medium** constraint (it was hard at weight
1); `required_skill_cost`'s default weight is now **1** (it was 10^9: the 10^9 moved into
the below-hard medium fold). Raw costs and the default score are unchanged. What changes
is an explicit weight or level in the engine: a weight on `required_skill_cost`, or on a
constraint made hard with `level="hard"`, is in hard units (one hard unit = 10^9 engine
units: `RequiredSkillCost(weight=1)` was 1 engine unit per missing skill and is now 10^9;
`CriticalPriorityTaskEndTimeCost(level="hard")` lands in the Bendable hard level, where it
used to decode as soft0); `RequiredSkillCost(level="soft")` is 1 soft unit per missing skill
(was 10^9). `UnassignedTasksCost(level="soft")` still scores one soft unit per task in the
engine. CP-SAT scores on the same three levels (`("hard", "medium", "soft")`) instead of
`hard, soft0, soft1, soft2`: the Bendable soft[1]-over-soft[2] order is carried by
`squared_makespan_cost`'s default weight 10^7 on the one soft level, as in the engine. There
`UnassignedTasksCost(level="soft")` now demotes to soft (it was a no-op on soft0), and
`RequiredSkillCost(level="soft")` now lands on the soft level (one soft unit per missing
skill, as in the engine); before, CP-SAT moved it to soft0. The folded CP-SAT objective no
longer fits one int64 for the demo, so CP-SAT optimizes the three levels one after another
(cpsat-common's staged solve).

**0.3.0:** every constraint takes two more knobs, `offset` and `clip` (keyword-only), and
may be picked several times. A raw cost `x` (per employee for the five unit constraints, on
the one total for `unassigned_tasks_cost`) is shaped as `sign(x) * max(0, |x| - offset) **
exponent` (`clip=True`, the default) or `(x - offset) ** exponent`, sign-preserving
(`clip=False`, which charges every employee, an idle one included, and acts as a reward
below the offset); `offset=0` is the 0.2.0 shaping. Each pick is its own instance with its
own knobs, and the instances add up:

```python
ta.solve(solution, constraints=[
    *get_constraints(),
    CriticalPriorityTaskEndTimeCost(exponent=2, offset=120),  # + (critical end times beyond 120)^2
])
```

`ta.constraint_costs(solution, picks)` keys the second pick
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

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-use-case).
The planning variable is **LIST**; the domain solver that configures the
engine is `cpp/include/ta/solver.hpp` + `cpp/src/solver.cpp`.

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
`cpp/include/ta/constraints/` (currently `required_skill_cost`, `squared_makespan_cost`,
`critical_priority_task_end_time_cost`, `major_priority_task_end_time_cost`,
`minor_priority_task_end_time_cost`, `unassigned_tasks_cost`; `common.hpp` holds the chain
walk and the priority-family base):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint`; positive raw cost = penalty, 0 = no violation) declaring
   `static constexpr double kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
   an `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `cpp/include/ta/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator`, weighted by the component's
   `kDefaultWeight` — `ta_plugin_test` checks they agree bit for bit — and a positive and a
   zero hand-calculated case to `ta_test` / `ta_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `python/task_assigning/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRuleCost(weight=5, level="hard")])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/task-assigning/cpsat`](../../benchmarks/task-assigning/cpsat) (a plugin
   subclassing the spec; defaults come from `constraint_info()`) so the benchmark stays like
   for like.

## Benchmark

[`benchmarks/task-assigning`](../../benchmarks/task-assigning) compares this use case with OR-Tools CP-SAT on the same instances.

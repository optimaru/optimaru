# optimaru.task_assigning (Python)

Skill-based task assignment, derived from the Timefold Quickstarts `task-assigning` example
(Apache-2.0; see the repository NOTICE): the pybind11 package of the `optimaru/task_assigning/cpp` implementation on
the generic C++ engine in `optimaru/cpp/optimaru/`. A **sequence-shaped** domain that fills an
ordered task list per employee (`Employee.tasks`); a task in no list is unassigned.

## Install and test

```bash
OPTIMARU_USE_CASES=task_assigning uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/task_assigning
```

## Usage

```python
from optimaru import task_assigning as ta
from optimaru.task_assigning.demo import generate_demo_data

solution = generate_demo_data()  # pydantic model (4 task types, 4 customers, 8 employees, 28 tasks)
result = ta.solve(solution, time_limit_seconds=10.0, random_seed=42)
print(result.score_hard, result.score_soft0, result.score_soft1, result.score_soft2)
for employee in result.employees:
    print(employee.full_name, employee.tasks)  # ordered list of task ids
```

Passing a hand-built `_core.TaskAssigningSolution` solves it in place.
For JSON serialization use `optimaru.task_assigning.models.TaskAssigningSolutionModel` (pydantic v2).

## JSON in, JSON out

`solve_request` takes a pydantic request, `models.TaskAssigningRequest`: the problem model as
`model_input`, with the settings of the solve under the names of `solve`'s keywords
(`time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`,
`full_assert`, and `solver_parameters` for further `SolverConfig` fields by name). It
returns the solved `TaskAssigningSolutionModel`, as `solve(model, ...)` does, and leaves the request
unchanged. `constraints=` is the selection `solve` takes.

```python
from optimaru.task_assigning import solve_request
from optimaru.task_assigning.models import TaskAssigningRequest

request = TaskAssigningRequest.model_validate_json(text)  # or TaskAssigningRequest(model_input=model, time_limit_seconds=5.0)
solved = solve_request(request)
```

## Domain and decision variables

The decision variable is each `Employee.tasks` — the **ordered list** of task indices
(ids in the model) the employee performs back to back. A task in no list is unassigned.
Derived values:

| derived value | rule |
|---|---|
| `task.employee` | the employee owning the list |
| `task.start_time` | the first task gets `min_start_time`, later ones `max(end of the previous task, min_start_time)` |
| duration | `base_duration x affinity multiplier` — HIGH 1 / MEDIUM 2 / LOW 3 / NONE 4 |
| employee endTime | end of the last task (0 for an empty list) |

The engine's construction heuristic assigns every task, and local search relocates them
while keeping them assigned — so unassigned (soft0) is always 0 in the final solution
(a task no skill matches still gets assigned with a hard penalty — see the caveat below).

## Score mapping (four levels, [1]hard/[3]soft -> engine Score)

This use case scores four levels, one hard and three soft (`hard[0]`, `soft[0]`, `soft[1]`,
`soft[2]`); the engine only
has `Score(int64 hard, double soft)`, and its constraints score on three levels
(hard, medium, soft). The mapping rule (`optimaru/task_assigning/cpp/include/ta/domain.hpp`):

```
hard[0] -> hard, soft[0] -> medium, soft[1] and soft[2] -> soft
engine.hard = hard  * 10^9 + soft0      # the medium level folds below hard (belowHard(10^9))
engine.soft = soft1 * 10^7 + soft2      # squared_makespan_cost's default weight is 10^7
```

Both are exact: the hard level in int64, the soft level within a double (exact up to 2^53).
`constraint_costs()` reports the fold on every entry
(`"medium_fold": {"kind": "below_hard", "unit": 1e9}`), and
`optimaru.task_assigning.constraints.score()` folds the same way.

Level priority holds while `|soft0| < 10^9` (task count), `|soft2| < 10^7`
(weighted endTime sum), `|soft1| < 9x10^8` (sum of endTime^2). At the default
weights and exponent 1 it stays exact up to horizons of tens of thousands of
minutes. The Python `Score` exposes all four levels (`hard`/`soft0`/`soft1`/`soft2`)
as-is; `soft1` / `soft2` are the soft level split by 10^7, so they are the two terms
only at the default weights.

Outside that range:

- `SquaredMakespanCost(exponent=2)` shapes the already squared cost again
  (endTime^4 per employee), which leaves the exact 2^53 window from an end time of
  about 173 minutes on one employee; a soft level beyond int64 raises
  `OverflowError` instead of decoding to nonsense.
- The `below_hard` fold leaves the hard level 1/10^9 of the int64 range: hard
  penalties above about 9.2x10^9 weighted units saturate at -(2^63 - 1). Keep
  `RequiredSkillCost` at weight 1 (the 10^9 between the levels is the fold), and give a soft term made hard a small weight
  (`SquaredMakespanCost(level="hard", weight=1)`).

## Constraints (6)

| Score level | Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|---|
| hard[0] | `required_skill_cost` | `RequiredSkillCost` | hard | 1 | required task skills the employee lacks |
| soft[1] | `squared_makespan_cost` | `SquaredMakespanCost` | soft | 10^7 | endTime^2 per employee (shortens late finishers first) |
| soft[2] | `critical_priority_task_end_time_cost` | `CriticalPriorityTaskEndTimeCost` | soft | 4 | endTime of CRITICAL tasks |
| soft[2] | `major_priority_task_end_time_cost` | `MajorPriorityTaskEndTimeCost` | soft | 2 | endTime of MAJOR tasks |
| soft[2] | `minor_priority_task_end_time_cost` | `MinorPriorityTaskEndTimeCost` | soft | 1 | endTime of MINOR tasks |
| soft[0] | `unassigned_tasks_cost` | `UnassignedTasksCost` | medium | 1 | 1 per unassigned task |

`list_constraints()` / `constraint_info()` order (unit constraints first, the global
`unassigned_tasks_cost` last). Pick, re-weight or flip them through
`optimaru.task_assigning.constraints` (`solve(solution, constraints=[RequiredSkillCost(), ...])`); a
weight is in units of its level, `level="hard"` makes a constraint hard (one hard unit =
10^9 engine units), `level="medium"` puts it on soft0 (one engine-hard unit, below the
missing skills, rounded per unit like hard), `level="soft"` makes it soft. The keyword-only
`offset=0.0` / `clip=True`
shift the shaping per employee (or on the global's total): `clip=True` counts only what
lies beyond the offset, `clip=False` shapes `x - offset` as it is. `clip=False` with an
offset is refused on hard and on medium, which counts toward the hard score here, because
hard 0 must mean no violation. A constraint may be
picked several times; each pick is its own instance with its own knobs, its
level included, the instances add up, and `constraint_costs(solution, picks)` (a `_core.TaskAssigningSolution`;
convert a model with `to_core()`) keys the k-th pick
`<name>#<k>`.

## Caveats

- **Engine feasibility**: the engine's internal `isFeasible` (engine.hard >= 0) requires
  `soft0 == 0` (every task assigned) on top of `hard == 0`. The final solution always
  assigns every task, so in practice feasible means `hard == 0`.
- **Keeping tasks unassigned**: hard dominates soft0, so leaving a task no employee's skills
  satisfy unassigned would score better than assigning it. Local search keeps
  assignments, though, so such a task gets assigned with a hard penalty. When every task type has
  a qualified employee, as in the demo data, there is no difference.
- `TaskType`/`Customer` references are expressed
  as indices (`_core`) or code/id strings (`models`).

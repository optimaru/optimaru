# task_assigning — task assigning (Timefold quickstart port)

Timefold quickstart [task-assigning](https://github.com/TimefoldAI/timefold-quickstarts)
(skill-based task assignment): the pybind11 package of the `../cpp` implementation ported onto
the generic C++ engine in `include/optimaru/`. A **LIST variable** domain that fills an
ordered task list per employee (Employee) (Java's `@PlanningListVariable(allowsUnassignedValues = true)`).

## Install / test

```bash
cd use-cases/task-assigning/python
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

## Usage

```python
import task_assigning as ta
from task_assigning.demo import generate_demo_data

solution = generate_demo_data()  # pydantic model (4 task types, 4 customers, 8 employees, 28 tasks)
result = ta.solve(solution, time_limit_seconds=10.0, random_seed=42)
print(result.score_hard, result.score_soft0, result.score_soft1, result.score_soft2)
for employee in result.employees:
    print(employee.full_name, employee.tasks)  # ordered list of task ids
```

Passing a hand-built `_core.TaskAssigningSolution` solves it in place.
For JSON serialization use `task_assigning.models.TaskAssigningSolutionModel` (pydantic v2).

## Domain and planning variables

The planning variable is each `Employee.tasks` — the **ordered list** of task indices
(ids in the model) the employee performs back to back. A task in no list is unassigned.
Shadow values are derived by the same rules as the Java original's shadow variables:

| shadow | rule |
|---|---|
| `task.employee` | the employee owning the list (`@InverseRelationShadowVariable`) |
| `task.start_time` | the first task gets `min_start_time`, later ones `max(end of the previous task, min_start_time)` (`startTimeSupplier`) |
| duration | `base_duration x affinity multiplier` — HIGH 1 / MEDIUM 2 / LOW 3 / NONE 4 |
| employee endTime | end of the last task (0 for an empty list) |

The engine's construction heuristic assigns every task, and local search relocates them
while keeping them assigned — so unassigned (soft0) is always 0 in the final solution
(a task no skill matches still gets assigned with a hard penalty; the Java original can
leave such a task unassigned — see the caveat below).

## Score mapping (BendableScore [1]hard/[3]soft -> engine HardSoftScore)

The original score type is `BendableScore(hardLevels=1, softLevels=3)`; the engine only
has `HardSoftScore(int64 hard, double soft)`. The mapping rule
(`cpp/include/ta/domain.hpp`, the same approach as the 10^9 offset precedent of
bed-allocation):

```
engine.hard = hard  * 10^9 + soft0      # both levels are integers, exact in int64
engine.soft = soft1 * 10^7 + soft2      # within a double (exact up to 2^53)
```

Level priority holds while `|soft0| < 10^9` (task count), `|soft2| < 10^7`
(weighted endTime sum), `|soft1| < 9x10^8` (sum of endTime^2). It stays exact up to
horizons of tens of thousands of minutes — the same practical range in which Java's
int-based BendableScore does not overflow. The Python `Score` exposes all four levels
(`hard`/`soft0`/`soft1`/`soft2`) as-is.

## Constraints (same semantics as Java `TaskAssigningConstraintProvider`, 6 of them)

| Level | Constraint | Penalty |
|---|---|---|
| hard[0] | noMissingSkills | required task skills the employee lacks |
| soft[0] | minimizeUnassignedTasks | 1 per unassigned task |
| soft[1] | minimizeMakespan | endTime^2 per employee (shortens late finishers first) |
| soft[2] | criticalPriorityTaskEndTime | endTime x 4 of CRITICAL tasks |
| soft[2] | majorPriorityTaskEndTime | endTime x 2 of MAJOR tasks |
| soft[2] | minorPriorityTaskEndTime | endTime x 1 of MINOR tasks |

## Caveats

- **Engine feasibility**: the engine's internal `isFeasible` (engine.hard >= 0) requires
  `soft0 == 0` (every task assigned) on top of `hard == 0`. The final solution always
  assigns every task, so in practice it matches Java's hard-level feasibility.
- **Keeping tasks unassigned**: the Java original can leave a task no employee's skills
  satisfy unassigned to keep hard 0 (hard dominates soft0). This port's local search keeps
  assignments, so such a task gets assigned with a hard penalty. When every task type has
  a qualified employee, as in the demo data, there is no difference.
- The `TaskType`/`Customer` object references of the Java demo's REST layer are expressed
  as indices (`_core`) or code/id strings (`models`).

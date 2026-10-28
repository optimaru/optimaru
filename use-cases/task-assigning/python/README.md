# task_assigning — 태스크 배정 (Timefold quickstart 포트)

Timefold quickstart [task-assigning](https://github.com/TimefoldAI/timefold-quickstarts)
(스킬 기반 태스크 배정)을 `include/optimaru/`의 범용 C++ 엔진 위에 포팅한 `../cpp` 구현의
pybind11 패키지. 직원(Employee)마다 순서 있는 태스크 리스트를 채우는
**LIST 변수** 도메인이다 (Java 의 `@PlanningListVariable(allowsUnassignedValues = true)`).

## 설치 / 테스트

```bash
cd use-cases/task-assigning/python
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

## 사용

```python
import task_assigning as ta
from task_assigning.demo import generate_demo_data

solution = generate_demo_data()  # pydantic 모델 (task type 4, customer 4, employee 8, task 28)
result = ta.solve(solution, time_limit_seconds=10.0, random_seed=42)
print(result.score_hard, result.score_soft0, result.score_soft1, result.score_soft2)
for employee in result.employees:
    print(employee.full_name, employee.tasks)  # 순서 있는 task id 리스트
```

`_core.TaskAssigningSolution` 을 직접 만들어 넘기면 in place 로 풀린다.
JSON 직렬화는 `task_assigning.models.TaskAssigningSolutionModel` (pydantic v2) 사용.

## 도메인과 planning variable

planning variable 은 각 `Employee.tasks` — 그 직원이 연달아 수행할 task
인덱스(모델에서는 id)의 **순서 있는 리스트**다. 어느 리스트에도 없는 task 는
미배정이다. shadow 값은 Java 원본의 shadow variable 체계와 같은 규칙으로
유도된다:

| shadow | 규칙 |
|---|---|
| `task.employee` | 리스트 소유 직원 (`@InverseRelationShadowVariable`) |
| `task.start_time` | 첫 task 는 `min_start_time`, 이후는 `max(이전 task 의 end, min_start_time)` (`startTimeSupplier`) |
| duration | `base_duration x affinity 배수` — HIGH 1 / MEDIUM 2 / LOW 3 / NONE 4 |
| 직원 endTime | 마지막 task 의 end (빈 리스트면 0) |

엔진의 construction heuristic 이 모든 task 를 배정하고 local search 는 배정을
유지한 채 재배치한다 — 그래서 최종 해에서 미배정(soft0)은 항상 0 이다
(스킬이 아예 안 맞는 task 도 hard 벌점을 받은 채 배정된다; Java 원본은 그런
task 를 미배정으로 남길 수 있다 — 아래 caveat).

## 점수 매핑 (BendableScore [1]hard/[3]soft -> 엔진 HardSoftScore)

원본 점수형은 `BendableScore(hardLevels=1, softLevels=3)`, 엔진은
`HardSoftScore(int64 hard, double soft)` 뿐이다. 매핑 규칙
(`cpp/include/ta/domain.hpp`, bed-allocation 의 10^9 오프셋 선례와 동일한
방식):

```
engine.hard = hard  * 10^9 + soft0      # 두 레벨 다 정수, int64 로 정확
engine.soft = soft1 * 10^7 + soft2      # double(2^53 까지 정확) 안에서
```

레벨 우선순위 보존 조건: `|soft0| < 10^9` (task 수), `|soft2| < 10^7`
(가중 endTime 합), `|soft1| < 9x10^8` (endTime^2 합). 분 단위 수만 분
horizon 규모까지 정확하다 — Java 의 int 기반 BendableScore 가 오버플로하지
않는 실용 범위와 같다. Python `Score` 는 4레벨(`hard`/`soft0`/`soft1`/
`soft2`) 그대로 노출된다.

## 제약 (Java `TaskAssigningConstraintProvider` 와 동일 의미론, 6개)

| 레벨 | 제약 | 벌점 |
|---|---|---|
| hard[0] | noMissingSkills | task 필수 스킬 중 직원에게 없는 수 |
| soft[0] | minimizeUnassignedTasks | 미배정 task 1건당 1 |
| soft[1] | minimizeMakespan | 직원별 endTime^2 (늦게 끝나는 직원 우선 단축) |
| soft[2] | criticalPriorityTaskEndTime | CRITICAL task 의 endTime x 4 |
| soft[2] | majorPriorityTaskEndTime | MAJOR task 의 endTime x 2 |
| soft[2] | minorPriorityTaskEndTime | MINOR task 의 endTime x 1 |

## Caveats

- **엔진 feasibility**: 엔진 내부 `isFeasible` (engine.hard >= 0) 은
  `hard == 0` 에 더해 `soft0 == 0` (전 태스크 배정)을 요구한다. 최종 해는
  항상 전 태스크가 배정되므로 실질적으로 Java 의 hard 레벨 feasibility 와
  일치한다.
- **미배정 유지**: Java 원본은 어떤 직원도 스킬을 만족 못 하는 task 를
  미배정으로 남겨 hard 0 을 지킬 수 있다 (hard 가 soft0 을 지배하므로).
  이 포트의 local search 는 배정을 유지하므로 그런 task 는 hard 벌점을 받은
  채 배정된다. 데모 데이터처럼 모든 task type 에 유자격 직원이 있으면 차이가
  없다.
- Java 데모 REST 층의 `TaskType`/`Customer` 객체 참조는 인덱스(`_core`) 또는
  code/id 문자열(`models`)로 표현된다.

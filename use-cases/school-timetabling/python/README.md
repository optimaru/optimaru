# st_timetabling

school-timetabling C++ 코어(`../cpp`, Timefold 스타일 엔진 `include/optimaru/` 기반)를
pybind11 로 감싼 Python 패키지. API_SPEC.md 의 canonical `TimetableProblem`
JSON 을 캐노니컬 C++ 도메인(`st::canon`)으로 옮겨 풀고, canonical
`Timetable` JSON 으로 돌려준다.

`st::canon` 은 CP-SAT 레퍼런스 패키지(`../../../benchmarks/school-timetabling/cpsat` 의
`teacher_scheduling`)와 **풀-패리티**다: 하드 H1~H10 / 소프트 S1~S6 전부를
`validate.py` 의미론 그대로 구현하며, 같은 해에 대해 두 백엔드의 하드 위반
수와 소프트 가중 벌점이 정확히 일치한다 — `tests/test_parity.py` 가 cpsat
해를 엔진에 주입해 이 패리티를 게이트로 고정한다.

## 설치 / 테스트

```bash
cd use-cases/school-timetabling/python
uv venv
uv pip install -e .
uv sync --all-groups   # pytest + pydantic + cpsat(패리티 게이트 ground truth)
uv run pytest -q
```

## 사용

```python
import st_timetabling as st

result = st.solve(problem_dict, time_limit_seconds=10.0, random_seed=42)
print(result["status"])  # FEASIBLE (hard 0) | UNKNOWN
print(result["penalties"])  # S1~S6 원시 카운트 (cpsat 과 같은 key)
print(result["objective_value"])  # 가중 벌점 합 (cpsat 과 같은 단위)
for lesson in result["lessons"]:
    print(lesson["course_id"], lesson["slot"], lesson["room_id"])
```

`weights=dict(...)` 로 소프트 가중치를, `config=`(모드별 `SolverConfig`
또는 `models.SolveConfig`) 로 탐색 옵션을 바꾼다. 저수준 API 는
`convert_problem` / `canon.Solver(canon.SolverConfig)` / `convert_solution` /
`inject_solution`(외부 해 주입, warm start·검증용), pydantic 직렬화 미러는
`st_timetabling.models` 에 있다.

## 플러그인 제약 조립 (`constraints=`)

기본 비용 함수는 모놀리식 인크리멘털 계산기(fast path)로 H1~H10 / S1~S6
전부를 계산한다. `constraints=` 를 주면 그 옆의 **플러그인 경로**로
전환된다: 제약을 컴포넌트 단위로 골라 weight 를 바꾸고, 하드/소프트 레벨을
뒤집고, 일부를 빼는 조립을 선언한다 .

```python
import st_timetabling as st
from st_timetabling.constraints import Assembly, list_constraints

print(list_constraints())  # 조립 가능한 제약 이름 15개 (H1, H3~H10, S1~S6)

result = st.solve(
    problem_dict,
    time_limit_seconds=10.0,
    # 기본 15개에서 시작해: S1 가중치 20 -> 40, S2 는 아예 제외.
    constraints=Assembly().add("teacher_gap", weight=40).drop("teacher_workday"),
)
```

es 처럼 스펙 인스턴스 리스트로 처음부터 조립할 수도 있고, 저수준
`canon.DecomposedCalculator` 로 직접 채점/설명할 수도 있다:

```python
from st_timetabling import canon
from st_timetabling.constraints import ClassConflict, TeacherConflict, TeacherGap

calc = canon.DecomposedCalculator([ClassConflict(), TeacherConflict(), TeacherGap(40)])
calc.add("teacher_load_limit", hard=False, weight=5)  # H9 를 soft 벌점으로 강등
score = calc.score(converted.table)  # 고정 배정 채점 (table.score 저장)
print(calc.constraint_totals())  # 제약별 가중 비용 (점수 설명)
```

규칙:

- 모든 스펙/`add` 는 `weight`(None = 컴포넌트 기본값: 하드 1, 소프트는
  cpsat 기본 가중치 — `solve` 에서는 `weights=` 인자가 그 기본을 대신한다),
  `hard`(True/False 로 기본 레벨 뒤집기), `exponent`(부호 보존 거듭제곱
  셰이핑)를 받는다. 잘못된 이름은 `ValueError`.
- `Assembly.add` 는 같은 이름이 이미 있으면 **교체**한다 (오버라이드
  의미론); 중복 합산이 필요하면 `canon.DecomposedCalculator.add` 를 쓴다.
- 기본 조립(`Assembly()`) + 기본 가중치는 모놀리식 채점과 **비트 단위로
  일치**한다 (`tests/test_constraints.py` 가 게이트).
- `solve` 응답의 `penalties` 는 항상 canonical 재채점(S1~S6 원시 카운트,
  조립과 무관)이고, `objective_value` 는 조립을 반영한 유효 S 가중치
  (오버라이드는 그 값, 제외한 항은 0)로 계산한다 — 그래서 S 계열 weight 는
  음이 아닌 정수여야 한다. `status` 도 canonical 하드 기준이라, 하드 제약을
  soft 로 강등한 조립이 그 제약을 위반한 채 끝나면 UNKNOWN 이 된다.
- **H1(`unassigned_lesson`)만은 예외다**: soft 로 내리면 미배정이 위반이
  아니라 **결과**가 된다 (cpsat 의 `WeeklyPeriodCounts(hard=false)` =
  `solve_partial` 과 같은 계약). 탐색에 미배정 무브가 붙어 "배정하지
  않는다" 가 선택지가 되고, 못 채운 시수는 `penalties["weekly_period_counts"]`
  로 보고되며 `status` 는 그 항을 뺀 하드로 정해진다.

  ```python
  # 못 채운 시수를 INFEASIBLE 대신 미배정으로 남긴다 (부분 편성).
  st.solve(problem_dict, constraints=Assembly().add("weekly_period_counts", hard=False, weight=10_000))
  ```

  미배정 1시간의 값이 곧 그 `weight` 다 — 다른 소프트 항 합보다 크게 둔다
  (cpsat 기본 10000). 이 조립에서는 `stop_at_first_feasible` 을 쓰면 안
  된다: 전부 빼 버린 해도 hard 0 이라 거기서 멈춘다.
- 플러그인 경로는 single-start 다 (`parallel_solver_count` 무시; 호출자
  소유 calculator 는 복제할 수 없다). 성능이 우선이면 기본 fast path 를
  그대로 쓴다.
- 플러그인 경로의 이동 처리량은 구조적 최적화(슬롯 계열 H3/H4/H5/H6/S5/S6
  의 델타 그룹 컴파일 + 교사 마스크 계열 H9/H10/S1~S3 의 공유 프로젝션)로
  모놀리식 대비 35~55% 수준까지 회복돼 있다 (track2 S/M/L, 10초 기준;
  나머지 격차는 범용 조립 디스패치의 고정 비용). 부분 조립·가중치
  오버라이드에도 그대로 적용된다.

## canonical 변환 (기본 모드)

- 강좌 하나는 `periods_per_week` 개의 엔진 Lesson 이 된다 (id `"{강좌}#k"`).
- 분반(`parallel_group`)은 주당 시수당 **병합 레슨 1개**로 접힌다: 교사
  합집합, 특별실 수요 합, 학급 점유 1 (cpsat 의 `occupancy_key` 와 동일).
  분반 동시 진행(H5)은 병합 자체로 성립하고, 멤버 과목별 선호/기피 교시는
  `preference_units` 로 각각 계수돼 S5/S6 도 정확히 일치한다. 역변환 시
  멤버 강좌별 canonical lesson 으로 다시 풀린다.
- `block_periods` >= 2 는 해당 강좌 레슨 중 그 수만큼에 blockGroup 을 부여해
  같은 요일 연속 교시(H8 하드)를 강제한다.
- `fixed_slots` -> pinned 레슨 (H2: 엔진이 아예 움직이지 않는다),
  `forbidden_slots` + 학급/교사 `unavailable` -> per-lesson allowed_slot
  테이블 (H5 하드).
- 팀티칭은 `teachers` 에 전원이 들어가 각자 충돌/상한/공강이 검사된다.
- `required_room_type` 은 (타입, 시간)별 수요 <= 보유 수의 하드 제약(H6)이고,
  구체적인 방은 cpsat 처럼 사후 그리디로 배정한다 — hard 0 해에서는 항상
  성공한다.
- 교사별 시수 상한(H9/H10)과 `preferred_free_days`(S3), 강좌 일일 상한(H7),
  `ObjectiveWeights` 전부 그대로 전달된다.

## 남은 한계

풀-패리티 이후 남는 것은 표현이 아니라 탐색/설계 성질이다:

- local search 는 최적성/비가능성을 **증명하지 못한다**: status 는
  FEASIBLE(hard 0) 또는 UNKNOWN 뿐이고 `best_objective_bound` 는 항상
  null 이다 (증명이 필요하면 cpsat 백엔드를 쓴다).
- 비트마스크 설계 한계: 교시 <= 16, 요일 index < 8 (초과 캘린더는
  ValueError 로 거부).
- hard 위반이 남은 해(UNKNOWN)에서는 미배정/특별실 부족이 `notes` 로
  보고될 수 있다. hard 0 해의 notes 는 항상 비어 있다.

## quickstart 모드 (`mode="quickstart"`)

quickstart 6제약 도메인(slotRoom 합성 인코딩)의 근사 변환은
`st_timetabling.quickstart` 로 남아 있다. 연강/분반/금지 시간/시수 상한/
선호 교시 등을 표현하지 못하고 soft 목적식도 엔진 내장 제약으로 대체된다 —
한계 전부가 응답 `notes` 에 명시된다. 자세한 내용은 quickstart 모듈
docstring 참고.

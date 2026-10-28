# project_job — 프로젝트 잡 스케줄링 (다중 프로젝트 RCPSP) Python 패키지

Timefold quickstart [project-job-scheduling](https://github.com/TimefoldAI/timefold-quickstarts/tree/stable/use-cases/project-job-scheduling)
(멀티 프로젝트 / 멀티 실행 모드 RCPSP) 을 `include/optimaru/` 의 범용 C++ 엔진 위에
포팅한 `../cpp` 구현의 pybind11 바인딩이다.

## 설치 / 테스트

```sh
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

## 사용

```python
import project_job as pj
from project_job.demo import generate_demo_data

schedule = generate_demo_data()  # pydantic 모델 (Java 데모 재현)
result = pj.solve(schedule, time_limit_seconds=10.0, random_seed=42)
print(result.score_hard, result.score_medium, result.score_soft)
for allocation in result.allocations:
    print(allocation.job_id, allocation.execution_mode_id, allocation.delay, allocation.start_date, allocation.end_date)
```

`solve` 는 `_core.ProjectJobSchedule`(in place) 또는
`models.ProjectJobScheduleModel`(JSON 직렬화용 pydantic 미러, id 참조)을
받는다. `stop_at_first_feasible=True` 는 hard 0 도달 즉시 종료.

## 도메인 (Java quickstart 그대로)

- **Project** — `release_date` + `critical_path_duration`. 지연 판정
  기준일(critical path 종료)에만 쓰이고 시작일을 강제하지 않는다.
- **Resource** — `renewable`(일자별 용량) / non-renewable(전체 기간 용량),
  글로벌(항상 renewable) 또는 프로젝트 로컬.
- **Job** — SOURCE / STANDARD / SINK. 실행 모드(기간 + 자원 요구량) 1개
  이상; SOURCE/SINK 는 기간 0 모드 1개.
- **Allocation** (planning entity) — job 과 1:1. Java 의 planning variable
  2개 `(executionMode, delay∈[0, 500))` 를 합성 인코딩한 BASIC 변수
  `mode_delay = delay * max_mode_count + mode_slot` 하나로 포팅했다
  (실제 mode index = `mode_slot % job 의 mode 수`, -1 == 미초기화).
  SOURCE/SINK 할당은 항상 pinned (Java 의 `@PlanningPin` + `setPinned`).
- **shadow 체인** — Java 의 `predecessorsDoneDate → startDate → endDate`
  shadow variable 은 파생 상태로 재현: 위상 정렬 순서로 재계산하고,
  증분 계산기는 변경 노드의 후속 서브그래프만 재전파한다.

## 제약 (HardMediumSoftScore, 전부 벌점)

| 제약 | 레벨 | 내용 |
|---|---|---|
| nonRenewableResourceCapacity | hard | non-renewable 자원별 전체 사용 합의 용량 초과분 |
| renewableResourceCapacity | hard | renewable 자원별 × 일자별 사용 합의 용량 초과분 |
| totalProjectDelay | medium | SINK 종료일 − critical path 종료일 (양수만) 의 합 |
| totalMakespan | soft | 최대 SINK 종료일 |

## 점수 매핑

Java 는 `HardMediumSoftScore`, 엔진은 `HardSoftScore(int64 hard, double
soft)` 만 안다. bed-allocation/meeting-scheduling 과 같은 오프셋 매핑을
쓴다:

```
engine.hard = hard
engine.soft = medium * 10^9 + soft      # kMediumUnit = 10^9
```

medium(지연 합)/soft(makespan)는 정수이고 double 은 2^53 까지 정확하므로
`|medium| < ~9.0x10^6`, `|soft| < 10^9` 인 한 사전식 순서가 보존된다
(둘 다 `할당 수 x (최대 delay + 최대 duration)` 로 바운드 — 현실적인
인스턴스에서 여유가 크다). 규칙 상세는 `../cpp/include/pjs/domain.hpp`.

## Java 원본과 다른 점 (caveats)

- **합성 인코딩**: 변수 2개 → 1개. mode 수가 `max_mode_count` 보다 적은
  job 은 여러 값이 같은 (mode, delay) 로 디코딩되는 중복 인코딩이 생기지만
  (`slot % mode 수`), 모든 값이 유효해 Java 의 per-entity value range
  의미론은 보존된다. 데모(STANDARD 전부 3모드)에서는 중복이 없다.
- **초기화 순서/휴리스틱**: Java 의 `DelayStrengthComparator`(delay 오름차순
  강도)는 엔진 CH 가 값을 0 부터 오름차순으로 스캔하는 것으로 동등하게
  재현된다 (별도 포팅 없음).
- **pinned 미초기화 할당**: 엔진 CH 는 pinning 을 모르는 채 미초기화
  엔티티를 전부 초기화하므로, `Solver.solve` 가 미초기화 pinned 할당을
  (첫 mode, delay 0) 으로 먼저 고정한다 (Java 데모의 SOURCE/SINK 관례).
- **Allocation 의 sourceAllocation/sinkAllocation 필드**: Java 에서 제약에
  쓰이지 않는 참조라 포팅하지 않았다 (REST 표현 전용).
- **데모 데이터**: 규모/분포는 Java `DemoDataGenerator` 그대로지만
  `java.util.Random(0)` 의 난수 스트림까지 재현하지는 않는다.

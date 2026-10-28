# bed_allocation — 병상 배정 (Timefold quickstart 포트)

Timefold quickstart [bed-allocation](https://github.com/TimefoldAI/timefold-quickstarts)
(병원 병상 배정, PAS)을 `include/optimaru/`의 범용 C++ 엔진 위에 포팅한 `../cpp` 구현의
pybind11 패키지. 재원(Stay)마다 병상(Bed) 하나를 고르는 BASIC 변수 도메인이다.

## 설치 / 테스트

```bash
cd use-cases/bed-allocation/python
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

## 사용

```python
import bed_allocation as ba
from bed_allocation.demo import generate_demo_data

plan = generate_demo_data()          # pydantic BedPlanModel (부서 1, 병실 10, 병상 13, 재원 ~75)
result = ba.solve(plan, time_limit_seconds=10.0, random_seed=42)
print(result.score_hard, result.score_medium, result.score_soft)
print([s.bed_id for s in result.stays][:5])
```

`_core.BedPlan` 을 직접 만들어 넘기면 in place 로 풀린다. JSON 직렬화는
`bed_allocation.models.BedPlanModel` (pydantic v2) 사용.

## 도메인과 planning variable

`Stay.bed` 하나가 planning variable 이다 (엔진 BASIC 인코딩):

| 값 | 의미 |
|---|---|
| `-1` | 미초기화 — 점수 기여 없음 (입력 상태) |
| `0..bed_count()-1` | 실제 병상 인덱스 |
| `bed_count()` (= `no_bed_value()`) | **"no bed" 센티널** — quickstart 의 null 병상 |

Java 원본의 `@PlanningVariable(allowsUnassigned = true)` 는 값 범위에
센티널 하나를 추가해 재현했다: 탐색이 재원을 "no bed" 로 옮길 수 있고,
그 재원은 medium 레벨로 박수만큼 벌점을 받는다. 그래서 hard 0 은 항상
도달 가능하다 (모든 hard 위반은 배정을 포기하는 것으로 풀 수 있으므로).

## 점수 매핑 (HardMediumSoftScore -> 엔진 HardSoftScore)

원본 점수형은 `HardMediumSoftScore`, 엔진은 `HardSoftScore(int64 hard,
double soft)` 뿐이다. 매핑 규칙 (`cpp/include/ba/domain.hpp`):

```
engine.hard = hard
engine.soft = medium * 10^9 + soft      # 전 제약이 벌점(<= 0)이고 정수
```

`|soft| < 10^9` 인 한 double(2^53 까지 정확) 안에서 medium 이 soft 를
사전식으로 지배한다 — soft 합의 상한은 약 `228 x 총 재원박수` 라서 수백만
재원박수 규모까지 정확하다. Python `Score` 는 3레벨 그대로 노출된다.

## 제약 (Java `BedAllocationConstraintProvider` 와 동일 의미론, 12개)

| 레벨 | 제약 | 벌점 |
|---|---|---|
| hard | sameBedInSameNight | 겹침 박수 x 1000 |
| hard | femaleInMaleRoom / maleInFemaleRoom | 박수 x 50 |
| hard | differentGenderInSameGenderRoomInSameNight | 겹침 박수 x 1000 |
| hard | departmentMinimumAge / departmentMaximumAge | 박수 x 100 |
| hard | requiredPatientEquipment | 박수 x (병실에 **있는** 필수 장비 수) x 50 |
| medium | assignEveryPatientToABed | "no bed" 재원의 박수 x 1 |
| soft | preferredMaximumRoomCapacity | 박수 x 8 |
| soft | departmentSpecialty | 박수 x 10 |
| soft | departmentSpecialtyNotFirstPriority | 박수 x (우선순위-1) x 10 |
| soft | preferredPatientEquipment | 박수 x (없는 선호 장비 수) x 50 |

주의점 (원본 그대로 유지한 것):

- **requiredPatientEquipment 는 병실에 '있는' 필수 장비 수로 가중한다.**
  필수 장비가 하나라도 모자라면 필터가 켜지고, 매치 가중치는 '없는' 수가
  아니라 '있는' 수다 — 전무한 병실은 벌점 0. 직관과 다르지만 Java 원본의
  유닛테스트(`penalizesBy(6)`)가 이 의미론을 고정하고 있어 그대로 포팅했다.
- departmentSpecialtyNotFirstPriority 는 부서가 그 전문과목을 제공할 때만
  적용된다. (Java 는 미제공이면 `getSpecialtyPriority()` 가 NPE 를 던지는
  코드라 그 경로의 의미론이 정의되어 있지 않다 — 여기서는 건너뛰고, 미제공은
  departmentSpecialty 벌점만 받는다.)
- 날짜는 정수 일 인덱스다 (`models` 는 `datetime.date.toordinal()` 사용).
  `night_count = departure - arrival + 1` (원본의 `+ 1` 유지).

## 데모

`bed_allocation.demo.generate_demo_data()` 는 Java `DemoDataGenerator` 를
재현한다: 부서 1개(전문과목 3개), 전부 SAME_GENDER 인 병실 10개(병상 13),
28박, 병실 타임라인당 순차 재원 ~75건, 원본과 같은 성별/연령/장비 분포.
(java.util.Random 스트림까지 동일하지는 않다 — 분포와 규모 기준.)
C++ 데모(`cpp/build/ba_demo`)는 10초 안에 hard 0 / medium 0 (전원 배정)에
도달한다.

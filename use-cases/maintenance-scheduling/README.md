# Maintenance Scheduling (C++ / Python)

Timefold quickstart [maintenance-scheduling](https://github.com/TimefoldAI/timefold-quickstarts/tree/stable/use-cases/maintenance-scheduling)
의 포트: 유지보수 작업(Job)에 작업 조(Crew)와 시작일을 배정해 조기/지연
유지보수를 최소화한다. `include/optimaru/` 의 범용 엔진(late acceptance + ALNS) 위에
C++ 도메인을 얹고, pybind11 로 Python 패키지(`maintenance_scheduling`)를
제공한다.

## 도메인 인코딩

Java 원본의 `Job` 은 planning variable 이 2개다 — `crew` 와 `startDate`
(주말 제외 TimeGrain 패턴) — 그리고 shadow variable `endDate` 를 가진다.
엔진은 entity 당 BASIC int 변수 1개를 지원하므로 school-timetabling 의
합성 인코딩 패턴을 따른다:

```
crewDate = startDateIndex * crewCount + crewIndex     # -1 == 미배정
```

- `startDateIndex` 는 달력 `[fromDate, toDate)` 의 주중(월~금) 목록
  (`MaintenanceSchedule::startDates`, Java `createStartDateList()`) 인덱스.
- shadow `endDate` 는 `(duration, startDateIndex)` 의 순수 함수라 리스너
  없이 prepare 단계에서 표(`endDateByDur`)로 선계산한다. Java
  `Job.calculateEndDate` 와 동일하게 주말을 건너뛴 exclusive 종료일이다.
- 날짜는 전부 epoch day (Java `LocalDate.toEpochDay`).

## 제약 (Java ConstraintProvider 전수 이식)

| 제약 | 레벨 | 벌점 (Java 와 동일 의미론) |
|---|---|---|
| crewConflict | Hard | 같은 crew 의 겹치는 작업쌍마다 -겹친 달력 일수 |
| minStartDate | Hard | minStartDate 보다 일찍 시작한 일수만큼 -1 |
| maxEndDate | Hard | maxEndDate 보다 늦게 끝난 일수만큼 -1 |
| beforeIdealEndDate | Soft | idealEndDate 보다 일찍 끝난 일수만큼 -1 |
| afterIdealEndDate | Soft | idealEndDate 보다 늦게 끝난 일수만큼 -1,000,000 |
| tagConflict | Soft | 겹치는 작업쌍의 공유 태그 수 x 겹친 일수만큼 -1,000 |

점수 매핑: Java 원본이 이미 `HardSoftScore` 라 엔진의
HardSoftScore(int64 hard, double soft)에 1:1 로 매핑되며 soft 가중치
(1 / 1,000 / 1,000,000)는 원본 그대로다. 두 pairwise 제약은 per-day
점유 카운트의 C(n,2) 합으로 환원되어 (구간이 연속이므로 쌍별 겹침 일수
합과 동치) 무브당 O(작업 달력 길이)로 증분 채점된다.

원본에 없는 확장: `Job::pinned` (엔진의 pinning 훅) — 점수에는 반영되지만
솔버가 절대 움직이지 않는다.

## 빌드 / 실행

```sh
cd use-cases/maintenance-scheduling/cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
./build/mnt_test          # 제약 손계산 + incremental==full + FULL_ASSERT
./build/mnt_demo          # Java 데모(SMALL) 재현, 10초, hard 0 도달 검증
./build/mnt_demo 15 --large

cd ../python
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

데모 데이터는 Java `DemoDataGenerator` 를 `java.util.Random(17)` 재현체로
비트 동일하게 재현한다 (SMALL: crew 3/8주/작업 14, LARGE: crew 5/16주/작업
48; 시작일은 다음 월요일). SMALL/LARGE 모두 수 초 안에 hard 0 에 도달한다.

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/maintenance-scheduling/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import maintenance_scheduling as mnt

schedule = mnt.generate_demo_data()
result = mnt.solve(schedule, time_limit_seconds=5.0, random_seed=42)
print(result.score)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`maintenance_scheduling.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/maintenance-scheduling/cpp -B build/maintenance-scheduling && cmake --build build/maintenance-scheduling -j
ctest --test-dir build/maintenance-scheduling
```

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/mnt/solver.hpp` + `cpp/src/solver.cpp`.

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
`cpp/include/mnt/constraints/` (currently `after_ideal_end_date`, `before_ideal_end_date`, `crew_conflict`, `max_end_date`, `min_start_date`, `tag_conflict`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/mnt/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `mnt_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/maintenance_scheduling/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/maintenance-scheduling/cpsat`](../../benchmarks/maintenance-scheduling/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/maintenance-scheduling`](../../benchmarks/maintenance-scheduling) compares this use case with
OR-Tools CP-SAT on the same instances, rescored by the same evaluator.

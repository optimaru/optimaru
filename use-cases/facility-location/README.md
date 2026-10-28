# Facility Location (설비 입지 선정)

Timefold quickstart [facility-location](https://github.com/TimefoldAI/timefold-quickstarts/tree/stable/use-cases/facility-location)
(Java/Quarkus) 를 이 레포의 C++ 엔진(`include/optimaru/`) 위에 포팅한 use case.
수요처(Consumer)마다 시설(Facility) 하나를 고르는 고전 FLP 다.

## 도메인 (BASIC 변수)

- `Consumer.facility` — 유일한 planning variable. `facilities` 인덱스,
  `-1` = 미배정 (Java 의 `@PlanningVariable Facility facility` null).
- `Facility` — Java 에선 `@InverseRelationShadowVariable(consumers)` 를 가진
  shadow entity 지만, 여기서는 집계(배정 수요·수요처 수)를 score calculator
  가 유지하므로 순수 problem fact 다.
- 거리 = `ceil(유클리드 도(degree) 거리 x 111km)` (Java `Location.getDistanceTo`).

## 제약 (HardSoftScore — 엔진 점수형 그대로, 매핑 불필요)

| 제약 | 수준 | 내용 |
|---|---|---|
| facility capacity | hard | 사용 시설별 `max(0, 배정 수요 합 - capacity)` 페널티 |
| facility setup cost | soft x2 | 수요처가 1개 이상 배정된 시설의 설치비 |
| distance from facility | soft x6 | 배정 수요처-시설 거리(m) |

거리 가중치 **6** 은 Java 솔루션의 `ConstraintWeightOverrides` 데모 값이다
(provider 선언은 `ofSoft(5)`, 솔루션이 `{"distance from facility": ofSoft(6)}`
으로 오버라이드). `FacilityLocationProblem.distanceFromFacilityWeight` /
`setupCostWeight` 필드로 조정할 수 있다 (기본 6 / 2 = quickstart 유효값).

## cpp/

- `include/fl/domain.hpp` — 도메인 + `optimaru::SolutionDescriptor` 특수화
- `score_calculator` — full recompute (정답 기준)
- `incremental_calculator` — before/after 훅, 무브당 O(1)
- `solver` — 엔진 래퍼 (CH + Late Acceptance + adaptive ruin&recreate)
- `src/main.cpp` — Java `DemoDataGenerator` 규모 재현 데모
  (시설 30 x 용량 150, 수요처 60 x 수요 15, 런던 박스, 설치비 N(50000, 10000²))

```bash
cd cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/fl_test      # 제약별 손계산 + 3000 랜덤 무브 증분==풀 + FULL_ASSERT solve
./build/fl_demo 10   # 10초 solve; hard 0 + 전 수요처 배정 확인 (exit 0)
```

## python/

pybind11 패키지 `facility_location` (모듈 `facility_location._core`).

```bash
cd python
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

```python
import facility_location as fl

problem = fl.FacilityLocationProblem(facilities, consumers)
fl.solve(problem, time_limit_seconds=5.0, random_seed=42)
print(problem.score, problem.used_facility_count(), problem.total_cost())
```

pydantic v2 직렬화 미러는 `facility_location.models`
(`FacilityLocationProblemModel.from_core / to_core`).

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/facility-location/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import facility_location as fl

facilities = [
    fl.Facility("F1", fl.Location(51.50, -0.12), 50_000, 300),   # id, location, setup cost, capacity
    fl.Facility("F2", fl.Location(51.52, -0.08), 40_000, 300),
]
consumers = [fl.Consumer(f"C{i}", fl.Location(51.49 + i * 0.005, -0.10), 30) for i in range(10)]
problem = fl.FacilityLocationProblem(
    facilities, consumers, fl.Location(51.44, -0.16), fl.Location(51.56, -0.01)
)
fl.solve(problem, time_limit_seconds=5.0, random_seed=42)          # solves in place
print(problem.score, problem.total_cost())
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`facility_location.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/facility-location/cpp -B build/facility-location && cmake --build build/facility-location -j
ctest --test-dir build/facility-location
```

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/fl/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`). `CloseFacilityMove` (empties a facility by moving all its consumers) is defined and registered in `cpp/src/solver.cpp` (`runEngine`) — use it as the template. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` next
to the engine construction in `cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** Entities are placed in index order (First Fit). Set
`engineConfig.constructionHeuristic.entityDifficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/fl/constraints/` (currently `distance_from_facility`, `facility_capacity`, `facility_setup_cost`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/fl/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `fl_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/facility_location/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/facility-location/cpsat`](../../benchmarks/facility-location/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/facility-location`](../../benchmarks/facility-location) compares this use case with
OR-Tools CP-SAT on the same instances, rescored by the same evaluator.

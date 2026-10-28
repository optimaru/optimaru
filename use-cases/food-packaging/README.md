# Food Packaging (C++ / Python)

Timefold quickstart [food-packaging](https://github.com/TimefoldAI/timefold-quickstarts/tree/stable/use-cases/food-packaging)의 C++ 엔진 포트.
포장 작업(Job)을 생산 라인(Line)에 **순서대로** 배정한다 — 제품이 바뀔 때마다
직전 제품에 따라 달라지는 청소(cleanup) 시간이 끼어들고, 같은 operator 가 맡은
라인들의 청소 구간은 겹치면 안 된다.

- `cpp/` — 도메인 + 제약(full/incremental) + 솔버 래퍼 + 데모/테스트.
  엔진은 `include/`(header-only)를 그대로 사용한다.
- `python/` — pybind11 바인딩(`food_packaging._core`)과 pydantic v2 미러.

## 모델 요약

| Java | 포트 |
|---|---|
| `Line.jobs` `@PlanningListVariable(allowsUnassignedValues=true)` | 엔진 LIST 변수. 미배정은 엔진에 리스트 하나로 더 노출되는 unassigned pool |
| `Line.operator` `@PlanningVariable` | 솔버 래퍼가 엔진 레그 사이에 힐클라이밍으로 재배정 (교대 최적화) |
| `Job` shadow 체인 (`startCleaning/startProduction/end`) | 리스트 위치에서 파생, 라인 단위 O(길이) 증분 재계산 |
| `@PlanningPin` | 라인의 pinned prefix 를 엔진 뷰에서 숨기는 구조적 고정 |
| `HardMediumSoftScore` | `engine.soft = medium * 10^10 + soft` 매핑 (도메인 헤더 주석 참고) |

## 빌드/실행

```sh
cd cpp && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
./build/fp_test          # 제약 손계산 + incremental==full + FULL_ASSERT + 데모 feasible
./build/fp_demo [초]     # Java 데모(라인 5, 작업 100) 재현, 기본 10초

cd ../python && uv venv && uv pip install -e . pytest pydantic && uv run pytest -q
```

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/food-packaging/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import food_packaging as fp

solution = fp.generate_demo_data()
result = fp.solve(solution, time_limit_seconds=5.0, random_seed=42)
print(result.score)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`food_packaging.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/food-packaging/cpp -B build/food-packaging && cmake --build build/food-packaging -j
ctest --test-dir build/food-packaging
```

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **LIST**; the domain solver that configures the
engine is `cpp/include/fp/solver.hpp` + `cpp/src/solver.cpp`.

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
`cpp/include/fp/constraints/` (currently `ideal_end_date_time`, `max_end_date_time`, `maximize_jobs_assigned`, `minimize_makespan`, `operator_cleaning_conflict`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/fp/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `fp_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/food_packaging/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/food-packaging/cpsat`](../../benchmarks/food-packaging/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/food-packaging`](../../benchmarks/food-packaging) compares this use case with
OR-Tools CP-SAT on the same instances, rescored by the same evaluator.

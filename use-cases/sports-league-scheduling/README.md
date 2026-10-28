# Sports League Scheduling (C++ / Python)

Timefold quickstart [sports-league-scheduling](https://github.com/TimefoldAI/timefold-quickstarts/tree/stable/use-cases/sports-league-scheduling)
을 `include/optimaru/` 의 범용 C++ 엔진 위에 포팅한 유스케이스. 리그의 모든 경기
(`Match`)에 라운드(`Round`)를 배정한다 (BASIC 변수 1개: `Match::round`).

- `cpp/` — 도메인(`sls::LeagueSchedule`), full-recompute 채점기, O(delta)
  인크리멘털 채점기, 얇은 솔버 래퍼(LA + ALNS), 데모(`sls_demo`),
  테스트(`sls_test`).
- `python/` — pybind11 패키지 `sports_league` (pydantic 미러, 데모 생성기,
  타입 스텁 포함). 상세는 `python/README.md`.

## 빌드 / 검증

```sh
cd cpp && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
./build/sls_test      # 제약 손계산 + incremental==full 3000무브 + FULL_ASSERT solve
./build/sls_demo 10   # Java 데모 재현(14팀/32라운드/182경기), 10초 solve, 0hard 도달 확인

cd ../python && uv venv && uv pip install -e . pytest pydantic && uv run pytest -q
```

## 점수 매핑

Java 원본이 이미 `HardSoftScore` 라 엔진의 `HardSoftScore(int64 hard,
double soft)` 로 항등 매핑 (soft 는 정수 값 그대로 double 에 저장).
제약 10개의 의미론은 Java `SportsLeagueSchedulingConstraintProvider` 와
동일하다 — 목록은 `python/README.md` 또는 `cpp/include/sls/
score_calculator.hpp` 주석 참고.

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/sports-league-scheduling/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import sports_league as sl

schedule = sl.demo_schedule()
sl.solve(schedule, time_limit_seconds=5.0, random_seed=42)
print(schedule.score)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`sports_league.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/sports-league-scheduling/cpp -B build/sports-league-scheduling && cmake --build build/sports-league-scheduling -j
ctest --test-dir build/sports-league-scheduling
```

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/sls/solver.hpp` + `cpp/src/solver.cpp`.

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
`cpp/include/sls/constraints/` (currently `away_to_away_hop`, `away_to_end_hop`, `away_to_home_hop`, `classic_matches`, `consecutive_away_matches`, `consecutive_home_matches`, `home_to_away_hop`, `matches_on_same_day`, `repeat_match_on_next_day`, `start_to_away_hop`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/sls/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `sls_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/sports_league/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/sports-league-scheduling/cpsat`](../../benchmarks/sports-league-scheduling/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/sports-league-scheduling`](../../benchmarks/sports-league-scheduling) compares this use case with
OR-Tools CP-SAT on the same instances, rescored by the same evaluator.

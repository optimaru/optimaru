# Conference Scheduling (Timefold quickstart 포팅)

Timefold quickstart [conference-scheduling](https://github.com/TimefoldAI/timefold-quickstarts/tree/stable/use-cases/conference-scheduling)
을 `include/optimaru/` 의 C++ 엔진 위에 포팅한 use case. 발표(`Talk`)마다 시간대와
발표장을 동시에 배정한다 — Java 의 두 `@PlanningVariable` (timeslot, room)
을 `slotRoom = timeslotIndex * roomCount + roomIndex` 합성 BASIC 변수 하나로
인코딩했다 (school-timetabling 과 같은 패턴).

- `cpp/` — 도메인(`cs::ConferenceSchedule`), full-recompute
  `ScoreCalculator`(+ 제약별 `Breakdown`), O(delta) 인크리멘털 계산기,
  ALNS 솔버 래퍼, Java `DemoDataGenerator` 재현 데모(`cs_demo`),
  테스트(`cs_test`).
- `python/` — pybind11 패키지 `conference_scheduling`
  (`_core` 확장 + pydantic 미러 + 데모 + pytest). 제약 목록/점수 매핑/
  value-range 대체(합성 `talkTypeCompatibility` hard 제약)의 자세한 설명은
  [python/README.md](python/README.md) 참고.

```sh
cd cpp && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
./build/cs_test && ./build/cs_demo 10
```

## 품질 벤치마크와 솔버 기본값 (2026-08)

`cpp/bench/quality_bench.cpp` (`cs_quality_bench <demo|scaled:N> <seed> <seconds>
[knobs]`) 가 데모형 스케일 인스턴스(고정 14슬롯 x 9룸 = 126칸, talk 수 가변)를
고정 wall-clock 예산으로 풀고 최종 `hard/soft`, 궤적, 제약별 breakdown,
moves/s 추정을 찍는다. N >= ~63 은 구조적으로 infeasible (랩 talk 수 > 랩 칸
수) 이라 hard 가 1차 지표, 45/60 은 feasible 이라 soft 가 지표다. 엔진 수치는
모놀리식 인크리멘털 계산기(fast path) 기준이다 (school-timetabling 과 같은
관례).

측정 (Apple Silicon macOS 단일 머신, 10초/런, seed 1-6 평균, 인터리브 A/B —
절대치는 머신에 따라 다르다):

| 인스턴스 | 튜닝 전 (hard / soft) | 튜닝 후 (hard / soft) | 시드별 승-패 |
| --- | --- | --- | --- |
| scaled:45 | 0 / -672575 | 0 / **-669730** (+0.4%) | 4-2 |
| scaled:60 | 0 / -1360055 | 0 / **-1348210** (+0.9%) | 4-2 |
| scaled:120 | -2783640 / -6978823 | **-2783333** / -6735535 (soft +3.5%) | 5-1 |
| scaled:200 | -10669418 / -24258323 | **-10667343** / -22919260 (soft +5.5%) | **6-0** |

바뀐 기본값/구조 두 가지:

- **인크리멘털 pairwise 델타의 집계화** — 예전에는 insert/retract 마다 모든
  talk 을 훑는 O(n) 스윕이었다 (change 무브당 2n, swap 당 4n). 지금은 슬롯별
  태그 점유 카운트(theme/sector/audience/content/mutex + theme x audience 조인트
  + (slot,room)별 theme + content x level + level/언어), (slot,room) 점유
  그리드(roomConflict), 용량 클래스별 favorite-rank Fenwick 트리(popularTalks),
  그리고 진짜 희소한 관계(연사 공유, prerequisite)만 인접 리스트로 남긴 정확
  집계 질의다. 모든 합이 정수라 점수는 비트 동일하고 (full-vs-incremental
  3000-무브 랜덤워크 / FULL_ASSERT / 플러그인 동등성 게이트로 고정), moves/s
  는 scaled:200 에서 ~7-10배, 품질은 위 표의 특히 120/200 이득의 대부분을
  만든다.
- **`ruinRecreateAsMoveWeight` 자동 기본값** — 파괴-재구성을 acceptor 판정을
  받는 평범한 무브로도 돌린다 (엔진 `RuinRecreateConfig::asMoveWeight`). 기본
  음수 = 자동 `0.2 / slotRoomCount` (benchmarks/school-timetabling/RESULTS.md 의 예산
  점유율 규칙; 이 도메인 126칸에서 1.6e-3). scaled:120 에서 가중치를 흔들면
  0.8e-3 / 3e-3 은 지고 1.6e-3 만 이겨 규칙값이 봉우리다. 단독 6시드 A/B 로
  네 크기 모두 soft +0.9~1.4%, 회귀 없음. `0` 을 주면 끈다.

측정만 하고 기본값은 끈 노브 (bench 키 = config 필드):

- `hot=` / `hotTalkProbability` — 하드 위반을 지닌 talk(타입 비호환 칸, 태그
  위반, 연사/룸 불가, 칸 공유 — O(1) 판정)을 rejection-sampling 으로 겨냥.
  scaled:120 에서 hard 4-2 승이지만 scaled:200 에서 1-5 패 (hot 집합이 talk
  의 70%+ 라 바이어스가 무뎌짐), 45/60 은 무승부. 크기별 규칙을 세우기엔
  측정점이 부족해 기본 0. feasibility 도달 자체는 빨라진다 (60 에서 84->19ms).
- `descent=` / `descentWeight` — ST 와 같은 이유로 기본 0 (미측정 아님:
  ST 에서 BASIC 하강은 as-move 보다 약했고, 여기서는 as-move 채택으로 대체).
- `la=` (`lateAcceptanceSize`) — 집계화로 스루풋이 3-10배 뛴 뒤에도
  480/2000(@120), 3200/6400(@200) 모두 자동값 `clamp(8 x talk, 800, 3200)` 을
  못 이겼다. 자동 유지.

원시 데이터/프로토콜: 캠페인 로그 (6시드 인터리브 A/B, 시드별 승패는
lexicographic hard-then-soft 비교).

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/conference-scheduling/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import conference_scheduling as cs

schedule = cs.demo_conference()
cs.solve(schedule, time_limit_seconds=5.0, random_seed=42)   # solves in place
print(schedule.score)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`conference_scheduling.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/conference-scheduling/cpp -B build/conference-scheduling && cmake --build build/conference-scheduling -j
ctest --test-dir build/conference-scheduling
```

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/cs/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`). This use case relies on the engine's generic moves only; it has no domain move yet. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` next
to the engine construction in `cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** `entityDifficulty` is already set in `cpp/src/solver.cpp` (talks are placed hardest-first). Set
`engineConfig.constructionHeuristic.entityDifficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/cs/constraints/` (currently `audience_level_diversity`, `audience_type_diversity`, `audience_type_theme_track_conflict`, `consecutive_talks_pause`, `content_audience_level_flow_violation`, `content_conflict`, `crowd_control`, `language_diversity`, `popular_talks`, `room_conflict`, `room_unavailable_timeslot`, `same_day_talks`, `sector_conflict`, `speaker_conflict`, `speaker_makespan`, `speaker_preferred_room_tags`, `speaker_preferred_timeslot_tags`, `speaker_prohibited_room_tags`, `speaker_prohibited_timeslot_tags`, `speaker_required_room_tags`, `speaker_required_timeslot_tags`, `speaker_unavailable_timeslot`, `speaker_undesired_room_tags`, `speaker_undesired_timeslot_tags`, `talk_mutually_exclusive_talks_tags`, `talk_preferred_room_tags`, `talk_preferred_timeslot_tags`, `talk_prerequisite_talks`, `talk_prohibited_room_tags`, `talk_prohibited_timeslot_tags`, `talk_required_room_tags`, `talk_required_timeslot_tags`, `talk_type_compatibility`, `talk_undesired_room_tags`, `talk_undesired_timeslot_tags`, `theme_track_conflict`, `theme_track_room_stability`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/cs/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `cs_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/conference_scheduling/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/conference-scheduling/cpsat`](../../benchmarks/conference-scheduling/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/conference-scheduling`](../../benchmarks/conference-scheduling) compares this use case with
OR-Tools CP-SAT on the same instances, rescored by the same evaluator.

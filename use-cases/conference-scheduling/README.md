# Conference Scheduling (C++ port)

The `conference-scheduling` example model (Apache-2.0; see the repository NOTICE)
ported onto the C++ engine in `include/optimaru/`. Every talk (`Talk`) gets a timeslot
and a room at once — Java's two `@PlanningVariable`s (timeslot, room) are encoded as
one composite BASIC variable `slotRoom = timeslotIndex * roomCount + roomIndex`
(the same pattern as school-timetabling).

- `cpp/` — domain (`cs::ConferenceSchedule`), full-recompute
  `ScoreCalculator` (+ per-constraint `Breakdown`), O(delta) incremental calculator,
  ALNS solver wrapper, a demo reproducing the Java `DemoDataGenerator` (`cs_demo`),
  tests (`cs_test`).
- `python/` — pybind11 package `conference_scheduling`
  (`_core` extension + pydantic mirror + demo + pytest). For the constraint list,
  score mapping and value-range replacement (the synthetic `talkTypeCompatibility`
  hard constraint) see [python/README.md](python/README.md).

```sh
cd cpp && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
./build/cs_test && ./build/cs_demo 10
```

## Quality benchmark and solver defaults (2026-08)

`cpp/bench/quality_bench.cpp` (`cs_quality_bench <demo|scaled:N> <seed> <seconds>
[knobs]`) solves demo-like scaled instances (a fixed 14 slots x 9 rooms = 126 cells,
variable talk count) on a fixed wall-clock budget and prints the final `hard/soft`, the
trajectory, the per-constraint breakdown and a moves/s estimate. N >= ~63 is structurally
infeasible (lab talks > lab cells), so hard is the primary metric there; 45/60 are
feasible, so soft is the metric. Engine figures use the monolithic incremental
calculator (fast path), the same convention as school-timetabling.

Measurements (one Apple Silicon macOS machine, 10 s per run, mean of seeds 1-6,
interleaved A/B — absolute values depend on the machine):

| Instance | Before tuning (hard / soft) | After tuning (hard / soft) | Per-seed win-loss |
| --- | --- | --- | --- |
| scaled:45 | 0 / -672575 | 0 / **-669730** (+0.4%) | 4-2 |
| scaled:60 | 0 / -1360055 | 0 / **-1348210** (+0.9%) | 4-2 |
| scaled:120 | -2783640 / -6978823 | **-2783333** / -6735535 (soft +3.5%) | 5-1 |
| scaled:200 | -10669418 / -24258323 | **-10667343** / -22919260 (soft +5.5%) | **6-0** |

Two changed defaults / structures:

- **Aggregated incremental pairwise deltas** — previously every insert/retract was an
  O(n) sweep over all talks (2n per change move, 4n per swap). Now they are exact
  aggregate queries: per-slot tag occupancy counts (theme/sector/audience/content/mutex +
  the theme x audience joint + per-(slot,room) theme + content x level + level/language),
  a (slot,room) occupancy grid (roomConflict), per-capacity-class favorite-rank Fenwick
  trees (popularTalks), and adjacency lists only for the truly sparse relations (shared
  speakers, prerequisites). Every sum is an integer, so scores are bit-identical (pinned
  by the full-vs-incremental 3000-move random walk / FULL_ASSERT / plugin equivalence
  gates); moves/s go up ~7-10x at scaled:200, which accounts for most of the quality gain
  in the table above, especially at 120/200.
- **Automatic `ruinRecreateAsMoveWeight` default** — ruin & recreate also runs as an
  ordinary move judged by the acceptor (engine `RuinRecreateConfig::asMoveWeight`). The
  default negative value = automatic `0.2 / slotRoomCount` (the budget-share rule of
  benchmarks/school-timetabling/RESULTS.md; 1.6e-3 for this domain's 126 cells). Sweeping
  the weight at scaled:120, 0.8e-3 / 3e-3 lose and only 1.6e-3 wins, so the rule value is
  the peak. A standalone 6-seed A/B gives soft +0.9-1.4% at all four sizes, no
  regressions. `0` turns it off.

Knobs that were measured but stay off by default (bench key = config field):

- `hot=` / `hotTalkProbability` — targets talks with hard violations (type-incompatible
  cell, tag violations, speaker/room unavailable, shared cell — O(1) checks) via rejection
  sampling. Wins hard 4-2 at scaled:120 but loses 1-5 at scaled:200 (the hot set is 70%+
  of talks, so the bias goes blunt); 45/60 tie. Too few data points for a per-size rule,
  so the default is 0. It does reach feasibility faster (84->19 ms at 60).
- `descent=` / `descentWeight` — default 0 for the same reason as ST (not unmeasured: in ST
  BASIC descent was weaker than as-move, and here as-move replaces it).
- `la=` (`lateAcceptanceSize`) — even after aggregation raised throughput 3-10x,
  480/2000 (@120) and 3200/6400 (@200) all failed to beat the automatic value
  `clamp(8 x talk, 800, 3200)`. Kept automatic.

Protocol: 6-seed interleaved A/B; per-seed win/loss is a lexicographic
hard-then-soft comparison. The raw campaign logs are not checked in.

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

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose search starts from `solution`, which is `ConferenceScheduleModel.from_core(schedule)` or that model as a JSON dict. `solve` takes the engine object, so convert the result with `to_core()`:

```python
from conference_scheduling.models import ConferenceScheduleModel

problem = ConferenceScheduleModel.from_core(cs.demo_conference())
solved = ConferenceScheduleModel.from_core(schedule)            # `schedule` solved above
restart = cs.with_initial_solution(problem, solved).to_core()
cs.solve(restart, time_limit_seconds=5.0, random_seed=43)
print(restart.score)
```

Talks are matched by code. A talk the solution leaves out is placed by the construction heuristic, and a complete solution is exactly where the search starts. Pinned talks keep their value on `problem`, whatever the solution says. `slot_room` is a composite over the timeslot and room lists, so a solution that lists different ones, or the same ones in another order, raises `ValueError`. So do unknown codes and out-of-range values, before anything reaches the engine, which does not range-check.

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

[`benchmarks/conference-scheduling`](../../benchmarks/conference-scheduling) compares this use case with OR-Tools CP-SAT on the same instances.

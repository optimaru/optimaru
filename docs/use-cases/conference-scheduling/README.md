# Conference Scheduling

Derived from the Timefold Quickstarts `conference-scheduling` example
(Apache-2.0; see the repository [NOTICE](../../../NOTICE)),
built on the C++ engine in `optimaru/cpp/optimaru/`. Every talk (`Talk`) gets a timeslot
and a room at once — the two decisions per talk (timeslot and room) are encoded as
one composite assignment variable `slotRoom = timeslotIndex * roomCount + roomIndex`
(the same pattern as school-timetabling).

- `optimaru/conference_scheduling/cpp/` — domain (`cs::ConferenceSchedule`), full-recompute
  `ScoreCalculator` (+ the C++ per-constraint `Breakdown`, one member per registry name),
  O(delta) incremental calculator, ALNS solver wrapper, a demo
  (`cs_demo`), tests (`cs_test`, `cs_plugin_test`).
  `optimaru/conference_scheduling/cpp/include/cs/constraints/` holds one component per constraint (`<subject>_cost.hpp`, plus
  `common.hpp`); `optimaru/conference_scheduling/cpp/include/cs/constraints.hpp` assembles the default set
  (`defaultConstraints()`).
- `optimaru/conference_scheduling/` — pybind11 package `conference_scheduling`
  (`_core` extension + pydantic mirror + demo + pytest). For the score and the
  talk-type compatibility rule (the `talk_type_incompatibility_cost` hard
  constraint) see [python.md](python.md).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPTIMARU_USE_CASES=conference_scheduling && cmake --build build -j4
./build/conference_scheduling/cs_test         # per-constraint hand checks + incremental==full + ScoreChecks::Full solve
./build/conference_scheduling/cs_plugin_test  # plugin assembly == monolith, raw costs by hand, catalog, guards
./build/conference_scheduling/cs_demo 10
```

## Usage

```sh
OPTIMARU_USE_CASES=conference_scheduling uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/conference_scheduling
```

```python
from optimaru import conference_scheduling as cs

schedule = cs.demo_conference()
cs.solve(schedule, time_limit_seconds=5.0, random_seed=42)   # solves in place
print(schedule.score)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`conference_scheduling.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -B build -DOPTIMARU_USE_CASES=conference_scheduling && cmake --build build -j
ctest --test-dir build
```

## Constraints

37 constraints: 17 hard and 20 soft. Every one is global (one
total per schedule), scored by default and weighed in minutes.

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (one global total; 0 = no violation; a reward is <= 0) |
|---|---|---|---|---|
| `talk_type_incompatibility_cost` | `TalkTypeIncompatibilityCost` | hard | 100'000 | sum of max(duration, 1) minutes for each talk whose timeslot does not admit its talk type, and again when its room does not |
| `room_unavailability_cost` | `RoomUnavailabilityCost` | hard | 100'000 | sum of the timeslot's duration in minutes for each talk in a room that is unavailable then |
| `room_conflict_cost` | `RoomConflictCost` | hard | 1'000 | sum of overlapping minutes for each pair of talks in the same room |
| `speaker_unavailability_cost` | `SpeakerUnavailabilityCost` | hard | 100 | sum of the timeslot's duration for each unavailable speaker of each talk (speaker-minutes) |
| `speaker_conflict_cost` | `SpeakerConflictCost` | hard | 10 | sum of shared speakers x overlapping minutes for each pair of overlapping talks |
| `talk_prerequisite_talks_cost` | `TalkPrerequisiteTalksCost` | hard | 10 | sum of the combined duration of both talks for each prerequisite that ends after its dependent talk starts |
| `talk_mutually_exclusive_talks_tags_cost` | `TalkMutuallyExclusiveTalksTagsCost` | hard | 1 | sum of shared mutually-exclusive tags x overlapping minutes for each pair of overlapping talks |
| `consecutive_talks_pause_cost` | `ConsecutiveTalksPauseCost` | hard | 1 | sum of the combined duration of both talks for each pair sharing a speaker without the minimum pause between them |
| `crowd_control_risk_cost` | `CrowdControlRiskCost` | hard | 1 | sum of the timeslot's duration for each risky talk (crowd_control_risk > 0) in a timeslot holding three or more risky talks |
| `speaker_required_timeslot_tags_cost` | `SpeakerRequiredTimeslotTagsCost` | hard | 1 | sum of required tags the timeslot lacks x its duration, per speaker of each talk |
| `speaker_prohibited_timeslot_tags_cost` | `SpeakerProhibitedTimeslotTagsCost` | hard | 1 | sum of prohibited tags the timeslot carries x its duration, per speaker of each talk |
| `talk_required_timeslot_tags_cost` | `TalkRequiredTimeslotTagsCost` | hard | 1 | sum of required tags the timeslot lacks x its duration, per talk |
| `talk_prohibited_timeslot_tags_cost` | `TalkProhibitedTimeslotTagsCost` | hard | 1 | sum of prohibited tags the timeslot carries x its duration, per talk |
| `speaker_required_room_tags_cost` | `SpeakerRequiredRoomTagsCost` | hard | 1 | sum of required tags the room lacks x the timeslot's duration, per speaker of each talk |
| `speaker_prohibited_room_tags_cost` | `SpeakerProhibitedRoomTagsCost` | hard | 1 | sum of prohibited tags the room carries x the timeslot's duration, per speaker of each talk |
| `talk_required_room_tags_cost` | `TalkRequiredRoomTagsCost` | hard | 1 | sum of required tags the room lacks x the timeslot's duration, per talk |
| `talk_prohibited_room_tags_cost` | `TalkProhibitedRoomTagsCost` | hard | 1 | sum of prohibited tags the room carries x the timeslot's duration, per talk |
| `theme_track_conflict_cost` | `ThemeTrackConflictCost` | soft | 10 | sum of shared theme tracks x overlapping minutes for each pair of overlapping talks |
| `theme_track_room_change_cost` | `ThemeTrackRoomChangeCost` | soft | 10 | sum of shared theme tracks x the combined duration for each same-day pair of talks in different rooms |
| `sector_conflict_cost` | `SectorConflictCost` | soft | 10 | sum of shared sectors x overlapping minutes for each pair of overlapping talks |
| `audience_type_theme_track_conflict_cost` | `AudienceTypeThemeTrackConflictCost` | soft | 1 | sum of shared theme tracks x shared audience types x overlapping minutes for each pair of overlapping talks |
| `content_audience_level_flow_violation_cost` | `ContentAudienceLevelFlowViolationCost` | soft | 10 | sum of shared content tags x the combined duration for each pair where the lower-level talk ends after the higher-level talk starts |
| `content_conflict_cost` | `ContentConflictCost` | soft | 100 | sum of shared content tags x overlapping minutes for each pair of overlapping talks |
| `different_day_talks_cost` | `DifferentDayTalksCost` | soft | 10 | sum of (shared content tags + shared theme tracks) x the combined duration for each related pair on different days |
| `popular_talks_room_size_cost` | `PopularTalksRoomSizeCost` | soft | 10 | sum of the combined duration for each pair where the talk with fewer favorites has the larger room |
| `speaker_preferred_timeslot_tags_cost` | `SpeakerPreferredTimeslotTagsCost` | soft | 20 | sum of preferred tags the timeslot lacks x its duration, per speaker of each talk |
| `speaker_undesired_timeslot_tags_cost` | `SpeakerUndesiredTimeslotTagsCost` | soft | 20 | sum of undesired tags the timeslot carries x its duration, per speaker of each talk |
| `talk_preferred_timeslot_tags_cost` | `TalkPreferredTimeslotTagsCost` | soft | 20 | sum of preferred tags the timeslot lacks x its duration, per talk |
| `talk_undesired_timeslot_tags_cost` | `TalkUndesiredTimeslotTagsCost` | soft | 20 | sum of undesired tags the timeslot carries x its duration, per talk |
| `speaker_preferred_room_tags_cost` | `SpeakerPreferredRoomTagsCost` | soft | 20 | sum of preferred tags the room lacks x the timeslot's duration, per speaker of each talk |
| `speaker_undesired_room_tags_cost` | `SpeakerUndesiredRoomTagsCost` | soft | 20 | sum of undesired tags the room carries x the timeslot's duration, per speaker of each talk |
| `talk_preferred_room_tags_cost` | `TalkPreferredRoomTagsCost` | soft | 20 | sum of preferred tags the room lacks x the timeslot's duration, per talk |
| `talk_undesired_room_tags_cost` | `TalkUndesiredRoomTagsCost` | soft | 20 | sum of undesired tags the room carries x the timeslot's duration, per talk |
| `speaker_makespan_cost` | `SpeakerMakespanCost` | soft | 20 | sum of (last day - first day - 1) x 480 minutes for each speaker whose talks span more than two consecutive days |
| `audience_type_diversity_cost` | `AudienceTypeDiversityCost` | soft (reward) | 1 | sum of -(shared audience types x the timeslot's duration) for each pair of talks in the same timeslot |
| `audience_level_diversity_cost` | `AudienceLevelDiversityCost` | soft (reward) | 1 | sum of -(the timeslot's duration) for each pair of talks in the same timeslot with different audience levels |
| `language_diversity_cost` | `LanguageDiversityCost` | soft (reward) | 10 | sum of -(the timeslot's duration) for each pair of talks in the same timeslot with different languages |

`conference_scheduling.constraint_info()` reports exactly this table from the C++
components (`kDefaultLevel` / `kDefaultWeight`), and the monolithic calculators read the
same constants. The three `*_diversity_cost` rows are **rewards**: their raw cost is <= 0,
they are never hard (`level="hard"` raises `ValueError` on both solvers) and their level is
not flippable (`level_flippable=False`). Every other constraint flips both ways
(`level="hard"` / `level="soft"`). `audience_type_diversity_cost` rewards same-timeslot pairs
that SHARE an audience type, despite its name.
`exponent` shapes the one total of each constraint (they are all global).

`constraint_costs(schedule)` is the one per-constraint surface (each reward reported
negated, as its raw cost <= 0), and `constraint_info()` is the catalog.

**Offsets and repeated picks.** Every constraint takes the knobs `weight`, `level`,
`exponent` and, keyword-only, `offset` and `clip`, and may be picked several times. Each constraint's one total `x` is shaped as
`sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which charges a total of 0 as
well and acts as a reward below the offset); `offset=0` is the plain power shaping. `clip=False`
with an offset is refused on hard (`RoomConflictCost` and the other hard constraints by
default), because hard 0 must mean no violation. Each pick is its own instance with its own
knobs, and the instances add up:

```python
cs.solve(schedule, constraints=[
    *get_constraints(),                                   # the 37 defaults
    ContentConflictCost(weight=1, exponent=2, offset=60), # + (content-conflict minutes beyond 60)^2
])
```

`cs.constraint_costs(schedule, picks)` keys the second pick `content_conflict_cost#2`. The
CP-SAT reference adds each plugin's terms as the one total the engine shapes, so it takes
integral offsets on every constraint.

`level` is a `Level` (`Level.HARD`, `Level.SOFT`, from `optimaru.conference_scheduling.constraints`) or its name.
Each repeated pick carries its own level, so `RoomConflictCost()` and
`RoomConflictCost(level="soft", offset=10)` charge the same room conflict once hard and once
soft beyond ten minutes. This use case has no medium level: `level="medium"` raises
`ValueError` before the solve starts, on both solvers and in `score()`.

## Quality benchmark and solver defaults

`optimaru/conference_scheduling/cpp/bench/quality_bench.cpp` (`cs_quality_bench <demo|scaled:N> <seed> <seconds>
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

- **Aggregated incremental pairwise deltas** — instead of an O(n) sweep over all talks
  per insert/retract (2n per change move, 4n per swap), they are exact aggregate
  queries: per-slot tag occupancy counts (theme/sector/audience/content/mutex +
  the theme x audience joint + per-(slot,room) theme + content x level + level/language),
  a (slot,room) occupancy grid (`room_conflict_cost`), per-capacity-class favorite-rank
  Fenwick trees (`popular_talks_room_size_cost`), and adjacency lists only for the truly sparse relations (shared
  speakers, prerequisites). Every sum is an integer, so scores are bit-identical (pinned
  by the full-vs-incremental 3000-move random walk / ScoreChecks::Full / plugin equivalence
  gates); moves/s go up ~7-10x at scaled:200, which accounts for most of the quality gain
  in the table above, especially at 120/200.
- **Automatic `ruinRecreateAsMoveWeight` default** — ruin & recreate also runs as an
  ordinary move judged by the acceptor (engine `RuinRecreateSettings::as_move_weight`). The
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
- `descent=` / `descent_weight` — default 0 for the same reason as ST (not unmeasured: in ST
  assignment-shape descent was weaker than as-move, and here as-move replaces it).
- `la=` (`history_length`) — even after aggregation raised throughput 3-10x,
  480/2000 (@120) and 3200/6400 (@200) all failed to beat the automatic value
  `clamp(8 x talk, 800, 3200)`. Kept automatic.

Protocol: 6-seed interleaved A/B; per-seed win/loss is a lexicographic
hard-then-soft comparison. The measurements are from 2026-08. The raw campaign logs are not checked in.

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose search starts from `solution`, which is `ConferenceScheduleModel.from_core(schedule)` or that model as a JSON dict. `solve` takes the engine object, so convert the result with `to_core()`:

```python
from optimaru.conference_scheduling.models import ConferenceScheduleModel

problem = ConferenceScheduleModel.from_core(cs.demo_conference())
solved = ConferenceScheduleModel.from_core(schedule)            # `schedule` solved above
restart = cs.with_initial_solution(problem, solved).to_core()
cs.solve(restart, time_limit_seconds=5.0, random_seed=43)
print(restart.score)
```

Talks are matched by code. A talk the solution leaves out is placed by the construction heuristic, and a complete solution is exactly where the search starts. Pinned talks keep their value on `problem`, whatever the solution says. `slot_room` is a composite over the timeslot and room lists, so a solution that lists different ones, or the same ones in another order, raises `ValueError`. So do unknown codes and out-of-range values, before anything reaches the engine, which does not range-check.

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **assignment** shape; the domain solver that configures the
engine is `optimaru/conference_scheduling/cpp/include/cs/solver.hpp` + `optimaru/conference_scheduling/cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`). This use case relies on the engine's generic moves only; it has no domain move yet. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.add_move_operator(...)` next
to the engine construction in `optimaru/conference_scheduling/cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** `difficulty` is already set in `optimaru/conference_scheduling/cpp/src/solver.cpp` (talks are placed hardest-first). Set
`engineConfig.construction.difficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`optimaru/conference_scheduling/cpp/include/cs/constraints/` (currently `talk_type_incompatibility_cost`, `room_unavailability_cost`, `room_conflict_cost`, `speaker_unavailability_cost`, `speaker_conflict_cost`, `talk_prerequisite_talks_cost`, `talk_mutually_exclusive_talks_tags_cost`, `consecutive_talks_pause_cost`, `crowd_control_risk_cost`, `speaker_required_timeslot_tags_cost`, `speaker_prohibited_timeslot_tags_cost`, `talk_required_timeslot_tags_cost`, `talk_prohibited_timeslot_tags_cost`, `speaker_required_room_tags_cost`, `speaker_prohibited_room_tags_cost`, `talk_required_room_tags_cost`, `talk_prohibited_room_tags_cost`, `theme_track_conflict_cost`, `theme_track_room_change_cost`, `sector_conflict_cost`, `audience_type_theme_track_conflict_cost`, `content_audience_level_flow_violation_cost`, `content_conflict_cost`, `different_day_talks_cost`, `popular_talks_room_size_cost`, `speaker_preferred_timeslot_tags_cost`, `speaker_undesired_timeslot_tags_cost`, `talk_preferred_timeslot_tags_cost`, `talk_undesired_timeslot_tags_cost`, `speaker_preferred_room_tags_cost`, `speaker_undesired_room_tags_cost`, `talk_preferred_room_tags_cost`, `talk_undesired_room_tags_cost`, `speaker_makespan_cost`, `audience_type_diversity_cost`, `audience_level_diversity_cost`, `language_diversity_cost`):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint` — here a `BreakdownConstraint` projecting one `Breakdown` member;
   positive raw cost = penalty, 0 = no violation, negative = reward) declaring
   `static constexpr double kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
   an `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter); a reward sets
   `reward = true; level_flippable = false;` in its constructor;
2. include it in `optimaru/conference_scheduling/cpp/include/cs/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to `score_calculator` (a `Breakdown`
   member of the same name, weighted in `hardTotal()` / `softPenaltyTotal()` /
   `softRewardTotal()` by the component's `kDefaultWeight`) and `incremental_calculator`
   — `cs_plugin_test` checks they agree bit for bit — and a positive and a zero
   hand-calculated case to `cs_test` / `cs_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `optimaru/conference_scheduling/constraints.py`
   (plus its module-level default instance); callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRuleCost(weight=5, level="hard")])`;
5. mirror it in the CP-SAT model, `external_solver.conference_scheduling` in
   [external-solver](https://github.com/optimaru/external-solver) (a plugin subclassing the
   spec; defaults come from `constraint_info()`), so the benchmark in
   [`benchmarks/conference-scheduling/cpsat`](../../../benchmarks/conference-scheduling/cpsat)
   stays like for like.

## Benchmark

[`benchmarks/conference-scheduling`](../../../benchmarks/conference-scheduling) compares this use case with OR-Tools CP-SAT on the same instances.

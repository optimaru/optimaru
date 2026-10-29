# conference_scheduling — Conference Scheduling (C++ engine + pybind11)

The `conference-scheduling` example model (Apache-2.0; see the repository NOTICE)
C++ port (`../cpp`, namespace `cs`) for use from Python.
Assigns every talk (`Talk`) a (timeslot, room) — `Talk.slot_room` is the
planning variable (-1 = unassigned; the value is the composite encoding
`timeslot_index * room_count + room_index`, the same pattern as school-timetabling).

## Install / test

```sh
uv venv
uv pip install -e . pytest pydantic
uv run pytest -q
```

## Usage

```python
import conference_scheduling as cs

schedule = cs.demo_conference()  # reproduces Java DemoDataGenerator (15 talks / 6 slots / 5 rooms)
cs.solve(schedule, time_limit_seconds=10.0, random_seed=42)
print(schedule.score)  # e.g. 0hard/-54420soft
print(cs.compute_breakdown(schedule).content_conflict)
```

- `solve(schedule, *, time_limit_seconds, random_seed, parallel_solver_count,
  stop_at_first_feasible, on_new_best, config)` — solves in place and returns it.
  `config` accepts a `SolverConfig` or a `models.SolveConfig`.
- `calculate_score` / `compute_breakdown` — full recompute of all 37 constraints
  and per-constraint match-unit totals (`hard_total`/`soft_penalty_total`/
  `soft_reward_total` apply the weights).
- Serialization is the pydantic v2 mirror in `conference_scheduling.models`
  (`ConferenceScheduleModel.from_core / to_core`, `SolveConfig`).

## Score mapping

The Java original already uses `HardSoftScore`, so it maps identically onto the engine's
`HardSoftScore(int64 hard, double soft)`: hard = minus the weighted violation-minute sum
(0 == feasible), soft = weighted rewards (diversity) − penalties (preferences/conflicts)
(always integer-valued, held in the engine's double field).

## Constraints (all of Java `ConferenceSchedulingConstraintProvider`, 16 hard + 20 soft)

hard: `roomUnavailableTimeslot` (100k/min) `roomConflict` (1k/overlap min)
`speakerUnavailableTimeslot` (100/min·speaker) `speakerConflict` (10/overlap min·shared speaker)
`talkPrerequisiteTalks` (10/summed min) `talkMutuallyExclusiveTalksTags`
`consecutiveTalksPause` `crowdControl` + 8 required/prohibited timeslot/room tag rules
(talk/speaker × required/prohibited × timeslot/room, 1/tag·min)

soft: `themeTrackConflict` `themeTrackRoomStability` `sectorConflict` (-10 each)
`audienceTypeDiversity`(+1) `audienceTypeThemeTrackConflict`(-1)
`audienceLevelDiversity`(+1) `contentAudienceLevelFlowViolation`(-10)
`contentConflict`(-100) `languageDiversity`(+10) `sameDayTalks`(-10)
`popularTalks` (-10) + 8 preferred/undesired tag rules (-20 each) + `speakerMakespan` (-20)

### Value range replacement (the only semantic difference)

Java restricts a Talk's value range to TalkType-compatible timeslots/rooms with
`@ValueRangeProvider`. This engine's BASIC variable is one global (timeslot × room)
grid, so the port gets the same effect with the synthetic hard constraint
`talkTypeCompatibility` (100k × max(min,1), per incompatible slot/room) — it never fires
on a solution that is feasible in Java, and the search is biased toward compatible
cells by `compatible_value_probability` (default 0.9).

## Demo data

`demo_conference()` reproduces the Java `DemoDataGenerator` exactly (down to the
java.util.Random(0) tag draws): 2 Lab + 13 Breakout talks, 6 timeslots in a day,
5 rooms, 12 speakers. A 10 s solve reaches hard 0.

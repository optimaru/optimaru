# conference_scheduling — Conference Scheduling (C++ engine + pybind11)

The conference-scheduling use case (C++ in `optimaru/conference_scheduling/cpp`, namespace `cs`)
for use from Python. Derived from the Timefold Quickstarts `conference-scheduling` example
(Apache-2.0; see the repository NOTICE).
Assigns every talk (`Talk`) a (timeslot, room) — `Talk.slot_room` is the
decision variable (-1 = unassigned; the value is the composite encoding
`timeslot_index * room_count + room_index`, the same pattern as school-timetabling).

## Install and test

```sh
OPTIMARU_USE_CASES=conference_scheduling uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/conference_scheduling
```

## Usage

```python
from optimaru import conference_scheduling as cs

schedule = cs.demo_conference()  # the demo instance (15 talks / 6 slots / 5 rooms)
cs.solve(schedule, time_limit_seconds=10.0, random_seed=42)
print(schedule.score)  # e.g. 0hard/-57990soft
print(cs.constraint_costs(schedule)["content_conflict_cost"]["costs"])  # raw, before the weight
```

- `solve(schedule, *, time_limit_seconds, random_seed, parallel_solver_count,
  stop_at_first_feasible, full_assert, on_new_best, config, constraints)` — a
  `ConferenceSchedule` is solved in place and returned; a `models.ConferenceScheduleModel` is left
  as it was and a new, solved model is returned. `config` accepts a `SolverConfig` or a `models.SolveConfig`;
  `constraints` picks the cost function (see below).
- `calculate_score` — full recompute of all 37 constraints at their default weights.
- `constraint_costs(schedule)` — every constraint's raw cost (one global total each;
  the three rewards report it negated, <= 0), before weight, exponent and level: the
  one per-constraint surface. `constraint_info()` — the catalog (default level and
  weight, `level_flippable`, `reward`, `default_enabled`), read off the C++ components.
- Serialization is the pydantic v2 mirror in `optimaru.conference_scheduling.models`
  (`ConferenceScheduleModel.from_core / to_core`, `SolveConfig`).

## Score

The score is the engine's
`Score(int64 hard, double soft)`: hard = minus the weighted violation-minute sum
(0 == feasible), soft = weighted rewards (diversity) − penalties (preferences/conflicts)
(always integer-valued, held in the engine's double field).

## Constraints (17 hard + 20 soft)

hard: `talk_type_incompatibility_cost` (100k/min, see below)
`room_unavailability_cost` (100k/min) `room_conflict_cost` (1k/overlap min)
`speaker_unavailability_cost` (100/min·speaker) `speaker_conflict_cost` (10/overlap min·shared speaker)
`talk_prerequisite_talks_cost` (10/summed min) `talk_mutually_exclusive_talks_tags_cost`
`consecutive_talks_pause_cost` `crowd_control_risk_cost` + 8 required/prohibited timeslot/room
tag rules (`<talk|speaker>_<required|prohibited>_<timeslot|room>_tags_cost`, 1/tag·min)

soft: `theme_track_conflict_cost` `theme_track_room_change_cost` `sector_conflict_cost` (-10 each)
`audience_type_diversity_cost` (+1, reward) `audience_type_theme_track_conflict_cost` (-1)
`audience_level_diversity_cost` (+1, reward) `content_audience_level_flow_violation_cost` (-10)
`content_conflict_cost` (-100) `language_diversity_cost` (+10, reward) `different_day_talks_cost` (-10)
`popular_talks_room_size_cost` (-10) + 8 preferred/undesired tag rules
(`<talk|speaker>_<preferred|undesired>_<timeslot|room>_tags_cost`, -20 each) +
`speaker_makespan_cost` (-20)

The full table (C++ component, default level and weight, raw cost) is in the
[use-case README](README.md#constraints);
`cs.constraint_info()` reports the same defaults. Pick, re-weight or flip constraints
with `optimaru.conference_scheduling.constraints`:

```python
from optimaru.conference_scheduling.constraints import ContentConflictCost, get_constraints, score

cs.constraint_info()
# [ConstraintInfo(name='talk_type_incompatibility_cost', default_level='hard',
#                 default_weight=100000.0, level_flippable=True, reward=False,
#                 default_enabled=True), ...]
picks = [ContentConflictCost(weight=500) if c.name == "content_conflict_cost" else c for c in get_constraints()]
cs.solve(schedule, time_limit_seconds=5.0, constraints=picks)
raw = cs.constraint_costs(schedule)       # re-priced in pure Python, no re-solve:
score(raw), score(raw, picks)
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping of the total (clip=True: only the minutes beyond 60 count, squared).
score(raw, [ContentConflictCost(), ContentConflictCost(weight=1, exponent=2, offset=60)])
```

The three `*_diversity_cost` rewards are never hard: `level="hard"` raises `ValueError`
(in `solve`, in `score()` and on the CP-SAT path).

Every constraint takes the knobs `weight`, `level`, `exponent` and, keyword-only, `offset`
and `clip`, and may be picked several times. Every constraint here is global, so the one total `x` is
shaped as `sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which also charges a total of 0
and rewards a total below the offset); `offset=0` is the plain power shaping. `clip=False` with an
offset is refused on hard (`ValueError: constraint 'room_conflict_cost': clip=False with a
non-zero offset cannot be hard, because hard 0 must mean no violation; use clip=True (a
threshold) or another level`), so a literal pick of a hard constraint goes on
`level="soft"`. Each pick is its own instance with its own knobs, the instances add up, and
`cs.constraint_costs(schedule, picks)` keys the second pick `content_conflict_cost#2`.
A literal offset on a reward (`AudienceTypeDiversityCost(offset=..., clip=False)`) shifts
the reward further; a threshold offset rewards only the part beyond it. `level` is
`Level.HARD` / `Level.SOFT` (or `"hard"` / `"soft"`), each repeated pick carries its own, and `level="medium"` raises
`ValueError`, as this use case has no medium level.

### Talk-type compatibility

A talk may only use timeslots and rooms that admit its talk type. The assignment variable
is one global (timeslot × room) grid, so this is enforced by the hard constraint
`talk_type_incompatibility_cost` (100k × max(min,1), per incompatible slot/room) rather than
by narrowing each talk's choices, and the search is biased toward compatible
cells by `compatible_value_probability` (default 0.9).

## Demo data

`demo_conference()` builds the demo conference, with
its tags drawn from a seeded `random.Random` (the same instance on every run): 2 Lab + 13 Breakout talks, 6 timeslots in a day,
5 rooms, 12 speakers. A 10 s solve reaches hard 0.

# meeting_scheduling

The `meeting-scheduling` example model (Apache-2.0; see the repository NOTICE)
C++ port (`../cpp`, generic engine in `include/optimaru`) with pybind11
bindings on top.

Assigns each meeting a starting time grain (15-minute units) and a room. The Java
entity's two planning variables (`startingTimeGrain`, `room`) become one BASIC variable
via the composite encoding `grain_room = time_grain_pos * room_count + room_index`
(-1 == unassigned), and a meeting occupies `duration_in_grains` consecutive grains from
its start — every overlap constraint is computed over that whole span.

## Constraints (same semantics as the Java ConstraintProvider)

| # | Level | Constraint (Python name) | C++ component | Default weight | Raw cost per unit (0 = no violation) |
|---|------|------|------|------|------------|
| 1 | hard | `room_conflict_cost` | `RoomConflictCost` | 1 | 1 per overlapping grain in the same room |
| 2 | hard | `overtime_cost` | `OvertimeCost` | 1 | lastTimeGrainIndex when the last grain index does not exist |
| 3 | hard | `required_attendance_conflict_cost` | `RequiredAttendanceConflictCost` | 1 | 1 per overlapping grain of pairs sharing a required attendee |
| 4 | hard | `room_capacity_cost` | `RoomCapacityCost` | 1 | seat shortfall (required+preferred − capacity) |
| 5 | hard | `day_crossing_cost` | `DayCrossingCost` | 1 | 1 when start/end days differ |
| 6 | medium | `required_and_preferred_attendance_conflict_cost` | `RequiredAndPreferredAttendanceConflictCost` | 1 (medium units) | 1 per overlapping grain of (required, preferred) pairs |
| 7 | medium | `preferred_attendance_conflict_cost` | `PreferredAttendanceConflictCost` | 1 (medium units) | 1 per overlapping grain of pairs sharing a preferred attendee |
| 8 | soft | `meeting_end_time_cost` | `MeetingEndTimeCost` | 1 | lastTimeGrainIndex |
| 9 | soft | `consecutive_meetings_cost` | `ConsecutiveMeetingsCost` | 100 | 1 per adjacent meeting pair |
| 10 | soft | `meeting_overlap_cost` | `MeetingOverlapCost` | 10 | 1 per overlapping grain of distinct meetings |
| 11 | soft | `larger_room_capacity_cost` | `LargerRoomCapacityCost` | 1 | capacity difference per larger room |
| 12 | soft | `room_change_cost` | `RoomChangeCost` | 1 | 1 when an attendee's consecutive (gap ≤ 2 grains) meetings change room |

`list_constraints()` orders them unit constraints first (2, 4, 5, 8, 11), then the
pair (global) ones; `constraint_info()` reports each default level and weight from the C++
components (`kDefaultLevel` / `kDefaultWeight`), and `constraint_costs(schedule)` the raw
per-unit costs (`meeting_scheduling.constraints.score()` turns them into a score). Every
constraint is flippable, none is a reward, all twelve are on by default. The old
`overtime_cost` quirk is kept: its raw cost is the last grain index, not the overrun.

**0.2.0:** every constraint gained the `_cost` suffix or a new name (`avoid_overtime` ->
`overtime_cost`, `required_room_capacity` -> `room_capacity_cost`, `start_and_end_on_same_day`
-> `day_crossing_cost`, `do_meetings_as_soon_as_possible` -> `meeting_end_time_cost`,
`one_break_between_consecutive_meetings` -> `consecutive_meetings_cost`,
`overlapping_meetings` -> `meeting_overlap_cost`, `assign_larger_rooms_first` ->
`larger_room_capacity_cost`, `room_stability` -> `room_change_cost`, the other four + `_cost`);
`compute_breakdown()` / `Breakdown` are removed in favour of `constraint_costs()`. The medium
pair now defaults to weight 1 in medium units (was 1e6 on soft): `level="hard"` gives one hard
unit per person-grain, `level="soft"` one soft unit (outside the fold).

### Score mapping (HardMediumSoftScore → HardSoftScore)

The engine score is `HardSoftScore(hard int64, soft double)`, so Java's medium level
is folded into soft:

```
hard = Java hard as-is (0 == feasible)
soft = -(medium penalty × 1e6 + raw soft penalty)     # MEDIUM_SCALE == 1e6
```

Level priority is preserved as long as the raw soft penalty (sum of grain indices,
100×adjacent pairs, 10×overlapping grains, capacity differences, room changes) stays
below 1e6 — always the case on realistic instances. The fold is declared once in C++
(`kMediumFold = intoSoft(1e6)` in `cpp/include/ms/domain.hpp`); both calculators and
`constraints.score()` (via the `medium_fold` on every `constraint_costs()` entry) apply it.

## Usage

```python
import meeting_scheduling as ms

schedule = ms.demo_data()  # reproduces Java DemoDataGenerator (24 meetings, 160 grains)
result = ms.solve(schedule, time_limit_seconds=10.0, random_seed=42)
raw = ms.constraint_costs(result)
medium = sum(sum(e["costs"]) * e["weight"] for e in raw.values() if e["level"] == "medium")
print(result.score, medium)

for a, v in zip(result.meeting_assignments, result.assignment()):
    pos, room = result.time_grain_pos_of(v), result.room_index_of(v)
    print(result.meetings[a.meeting].topic, result.time_grains[pos].id, result.rooms[room].name)
```

The pydantic mirror (serialization) is `meeting_scheduling.models`; per-constraint
raw costs come from `constraint_costs` (the Java ConstraintVerifier tests are ported onto
these costs, in `tests/test_constraints.py` and the C++ tests).

Picking the cost function (see `meeting_scheduling.constraints`):

```python
from meeting_scheduling.constraints import MeetingOverlapCost, RoomConflictCost, get_constraints, score

ms.solve(schedule, time_limit_seconds=1.0, constraints=[RoomConflictCost(), MeetingOverlapCost(weight=100)])
raw = ms.constraint_costs(schedule)       # re-priced in pure Python, no re-solve:
score(raw), score(raw, [RoomConflictCost(), MeetingOverlapCost(weight=100)])
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping (clip=True: only the overlap beyond 8 grains counts, squared).
score(raw, [*get_constraints(), MeetingOverlapCost(weight=1, exponent=2, offset=8)])
```

**0.3.0:** every constraint takes two more knobs, `offset` and `clip` (keyword-only), and
may be picked several times. A raw cost `x` (per (grain, room) cell for the five
per-meeting constraints, the one total for the seven pair constraints) is shaped as
`sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which charges every cell, an
empty one included, and rewards a cost below the offset); `offset=0` is the 0.2.0 shaping.
Each pick is its own instance with its own knobs, the instances add up, and
`ms.constraint_costs(schedule, picks)` keys the second pick `meeting_overlap_cost#2`.
The `hard` knob is gone (`hard=` raises `TypeError`): `level` replaces it (`Level.HARD` /
`Level.MEDIUM` / `Level.SOFT`, or the names), any constraint may move onto medium (its
weight then in medium units, folded at 1e6), and each repeated pick carries its own level.

## Build / test

```sh
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

## Differences from the original (caveats)

- Meetings and `MeetingAssignment`s must be 1:1 (the quickstart demo model;
  `ValueError` otherwise).
- The composite encoding has no partial state like "time grain assigned, room null".
  Java constraints built on `forEachIncludingUnassigned` (e.g. the room capacity rule,
  `room_capacity_cost`, with room == null) score differently only in that state; on fully assigned solutions the
  semantics match exactly. In this port an unassigned (-1) entity contributes no score
  (engine contract).
- `demo_data()` reproduces `java.util.Random(0)` and makes the same draws as the Java
  demo, but pins the calendar dates to dayOfYear 200..203 (Java uses 4 days from
  "tomorrow").

## Solver defaults and quality bench (2026-08-18)

The default search settings were tuned with `cpp/bench/ms_quality_bench.cpp`
(monolithic incremental fast path, 10 s / single thread, 6-12 seeds interleaved
A/B; absolute values are from one Apple Silicon macOS machine and vary per machine).
The objective is `medium penalty × 1e6 + raw soft` (given hard 0; lower is better).

Adopted defaults (full history in the comments of `cpp/include/ms/solver.hpp`):

- `late_acceptance_size = 0` → automatic `clamp(128 × assignments, 3200, 6400)`
- `day_fit_value_probability = 0.8` (steers change moves to grains that fit within a day)
- `ruin_recreate_as_move_weight = 2e-4`, `ruin_recreate_as_move_max_ruin = 12`
  — destroy-repair is also drawn as an ordinary move judged by the acceptor. A per-step
  probability of 2e-4 was a flat, size-independent peak (1e-4/3e-4/5e-4 all worse).
- The incremental calculator's delta path is vectorized with a two-pass sum/increment
  plus a pre-decoded cache (score semantics unchanged; moves/s only, +25-35%).

Mean of seeds 101..106 in the 2026-08-18 round (10 s):

| Instance | Previous defaults | Current defaults | Δ |
| --- | --- | --- | --- |
| demo (24 meetings, 480 cells) | 3,059 | 3,049 | −0.3% (neutral) |
| scaled:48 (960 cells) | 33,681,990 | 31,015,587 | −7.9% |
| scaled:96 (1920 cells) | 311,564,460 | 274,069,113 | −12.0% |

Measured and rejected (the do-not-retry list is also in the `solver.hpp` comments): as-move
ruin 6/24, fixed LA 3200/12800, polish 2000/15000, change/swap 0.85/0.15,
dayFit 0.9, smaller kick ruin (0.05-0.15).

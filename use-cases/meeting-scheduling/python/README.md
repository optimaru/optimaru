# meeting_scheduling

Timefold quickstart [meeting-scheduling](https://github.com/TimefoldAI/timefold-quickstarts/tree/stable/use-cases/meeting-scheduling)
C++ port (`../cpp`, generic engine in `include/optimaru`) with pybind11
bindings on top.

Assigns each meeting a starting time grain (15-minute units) and a room. The Java
entity's two planning variables (`startingTimeGrain`, `room`) become one BASIC variable
via the composite encoding `grain_room = time_grain_pos * room_count + room_index`
(-1 == unassigned), and a meeting occupies `duration_in_grains` consecutive grains from
its start — every overlap constraint is computed over that whole span.

## Constraints (same semantics as the Java ConstraintProvider)

| # | Level | Constraint | Penalty per unit |
|---|------|------|------------|
| 1 | hard | roomConflict | 1 per overlapping grain in the same room |
| 2 | hard | avoidOvertime | lastTimeGrainIndex when the last grain index does not exist |
| 3 | hard | requiredAttendanceConflict | 1 per overlapping grain of pairs sharing a required attendee |
| 4 | hard | requiredRoomCapacity | seat shortfall (required+preferred − capacity) |
| 5 | hard | startAndEndOnSameDay | 1 when start/end days differ |
| 6 | medium | requiredAndPreferredAttendanceConflict | 1 per overlapping grain of (required, preferred) pairs |
| 7 | medium | preferredAttendanceConflict | 1 per overlapping grain of pairs sharing a preferred attendee |
| 8 | soft | doMeetingsAsSoonAsPossible | lastTimeGrainIndex |
| 9 | soft | oneBreakBetweenConsecutiveMeetings | 100 per adjacent meeting pair |
| 10 | soft | overlappingMeetings | 10 per overlapping grain of distinct meetings |
| 11 | soft | assignLargerRoomsFirst | capacity difference per larger room |
| 12 | soft | roomStability | 1 when an attendee's consecutive (gap ≤ 2 grains) meetings change room |

### Score mapping (HardMediumSoftScore → HardSoftScore)

The engine score is `HardSoftScore(hard int64, soft double)`, so Java's medium level
is folded into soft:

```
hard = Java hard as-is (0 == feasible)
soft = -(medium penalty × 1e6 + raw soft penalty)     # MEDIUM_SCALE == 1e6
```

Level priority is preserved as long as the raw soft penalty (sum of grain indices,
100×adjacent pairs, 10×overlapping grains, capacity differences, room changes) stays
below 1e6 — always the case on realistic instances.

## Usage

```python
import meeting_scheduling as ms

schedule = ms.demo_data()  # reproduces Java DemoDataGenerator (24 meetings, 160 grains)
result = ms.solve(schedule, time_limit_seconds=10.0, random_seed=42)
print(result.score, ms.compute_breakdown(result).medium_total())

for a, v in zip(result.meeting_assignments, result.assignment()):
    pos, room = result.time_grain_pos_of(v), result.room_index_of(v)
    print(result.meetings[a.meeting].topic, result.time_grains[pos].id, result.rooms[room].name)
```

The pydantic mirror (serialization) is `meeting_scheduling.models`; per-constraint
violation counts come from `compute_breakdown` (the Java ConstraintVerifier tests are
ported onto these counts).

## Build / test

```sh
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

## Differences from the original (caveats)

- Meetings and `MeetingAssignment`s must be 1:1 (the quickstart demo model;
  `ValueError` otherwise).
- The composite encoding has no partial state like "time grain assigned, room null".
  Java constraints built on `forEachIncludingUnassigned` (e.g. requiredRoomCapacity with
  room == null) score differently only in that state; on fully assigned solutions the
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

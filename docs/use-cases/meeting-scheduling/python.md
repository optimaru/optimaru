# meeting_scheduling

The C++ implementation of the meeting-scheduling use case
(`optimaru/meeting_scheduling/cpp`, generic engine in `optimaru/cpp/optimaru`) with pybind11
bindings on top. Derived from the Timefold Quickstarts `meeting-scheduling` example
(Apache-2.0; see the repository NOTICE).

Assigns each meeting a starting time grain (15-minute units) and a room. The two
are one decision variable per meeting,
the composite encoding `grain_room = time_grain_pos * room_count + room_index`
(-1 == unassigned), and a meeting occupies `duration_in_grains` consecutive grains from
its start — every overlap constraint is computed over that whole span.

## Constraints

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
per-unit costs (`optimaru.meeting_scheduling.constraints.score()` turns them into a score). Every
constraint is flippable, none is a reward, all twelve are on by default. Note that
`overtime_cost`'s raw cost is the last grain index, not the overrun.

The medium pair defaults to weight 1 in medium units: `level="hard"` gives one hard unit per
person-grain, `level="soft"` one soft unit (outside the fold).

### Score mapping (hard / medium / soft → Score)

The engine score is `Score(hard int64, soft double)`, so the medium level
is folded into soft:

```
hard = the hard level as-is (0 == feasible)
soft = -(medium penalty × 1e6 + raw soft penalty)     # MEDIUM_SCALE == 1e6
```

Level priority is preserved as long as the raw soft penalty (sum of grain indices,
100×adjacent pairs, 10×overlapping grains, capacity differences, room changes) stays
below 1e6 — always the case on realistic instances. The fold is declared once in C++
(`kMediumFold = intoSoft(1e6)` in `optimaru/meeting_scheduling/cpp/include/ms/domain.hpp`); both calculators and
`constraints.score()` (via the `medium_fold` on every `constraint_costs()` entry) apply it.

## Usage

```python
from optimaru import meeting_scheduling as ms

schedule = ms.demo_data()  # the demo instance (24 meetings, 160 grains)
result = ms.solve(schedule, time_limit_seconds=10.0, random_seed=42)
raw = ms.constraint_costs(result)
medium = sum(sum(e["costs"]) * e["weight"] for e in raw.values() if e["level"] == "medium")
print(result.score, medium)

for a, v in zip(result.meeting_assignments, result.assignment()):
    pos, room = result.time_grain_pos_of(v), result.room_index_of(v)
    print(result.meetings[a.meeting].topic, result.time_grains[pos].id, result.rooms[room].name)
```

The pydantic mirror (serialization) is `optimaru.meeting_scheduling.models`; per-constraint
raw costs come from `constraint_costs` (the per-constraint tests assert on
these costs, in `tests/meeting_scheduling/test_constraints.py` and the C++ tests).

Picking the cost function (see `optimaru.meeting_scheduling.constraints`):

```python
from optimaru.meeting_scheduling.constraints import MeetingOverlapCost, RoomConflictCost, get_constraints, score

ms.solve(schedule, time_limit_seconds=1.0, constraints=[RoomConflictCost(), MeetingOverlapCost(weight=100)])
raw = ms.constraint_costs(schedule)       # re-priced in pure Python, no re-solve:
score(raw), score(raw, [RoomConflictCost(), MeetingOverlapCost(weight=100)])
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping (clip=True: only the overlap beyond 8 grains counts, squared).
score(raw, [*get_constraints(), MeetingOverlapCost(weight=1, exponent=2, offset=8)])
```

Besides `weight`, `level` and `exponent`, every constraint takes `offset` and `clip`
(keyword-only), and may be picked several times. A raw cost `x` (per (grain, room) cell for the five
per-meeting constraints, the one total for the seven pair constraints) is shaped as
`sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which charges every cell, an
empty one included, and rewards a cost below the offset); `offset=0` is the plain shaping.
`clip=False` with an offset is refused on hard (`ValueError: constraint 'overtime_cost':
clip=False with a non-zero offset cannot be hard, because hard 0 must mean no violation; use
clip=True (a threshold) or another level`); on soft and on medium (folded into soft) it is
allowed. Each pick is its own instance with its own knobs, the instances add up, and
`ms.constraint_costs(schedule, picks)` keys the second pick `meeting_overlap_cost#2`.
`level` is `Level.HARD` / `Level.MEDIUM` / `Level.SOFT` (or the names), any constraint may move onto medium (its
weight then in medium units, folded at 1e6), and each repeated pick carries its own level.

## Install and test

```sh
OPTIMARU_USE_CASES=meeting_scheduling uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/meeting_scheduling
```

## Caveats

- Meetings and `MeetingAssignment`s must be 1:1 (`ValueError` otherwise).
- The composite encoding has no partial state like "time grain assigned, room unassigned":
  an assignment has both or neither, and an unassigned (-1) assignment contributes no score
  (engine contract).
- `demo_data()` is deterministic: its draws come from a seeded `std::mt19937_64` and the
  calendar dates are pinned to dayOfYear 200..203.

## Solver defaults and quality bench

The default search settings were tuned with `optimaru/meeting_scheduling/cpp/bench/ms_quality_bench.cpp`
(monolithic incremental fast path, 10 s / single thread, 6-12 seeds interleaved
A/B; absolute values are from one Apple Silicon macOS machine and vary per machine).
The objective is `medium penalty × 1e6 + raw soft` (given hard 0; lower is better).

Adopted defaults (full history in the comments of `optimaru/meeting_scheduling/cpp/include/ms/solver.hpp`):

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

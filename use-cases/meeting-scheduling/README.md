# Meeting Scheduling

Derived from the Timefold Quickstarts `meeting-scheduling` example (Apache-2.0), translated
and modified; see the repository [NOTICE](../../NOTICE).

Domain model, constraints and API details: [`python/`](python/README.md).

## Usage

```sh
cd use-cases/meeting-scheduling/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import meeting_scheduling as ms

schedule = ms.demo_data()
result = ms.solve(schedule, time_limit_seconds=5.0, random_seed=42)
print(result.score.hard, result.score.soft)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`meeting_scheduling.SolverConfig`) and `constraints` (see below).

## Constraints

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `overtime_cost` | `OvertimeCost` | hard | 1 | per meeting running past the calendar: its last grain index (not the overrun) |
| `room_capacity_cost` | `RoomCapacityCost` | hard | 1 | per meeting: `max(0, required + preferred attendees - room capacity)` seats |
| `day_crossing_cost` | `DayCrossingCost` | hard | 1 | per meeting: time grains at its last grain index on another day than its start |
| `meeting_end_time_cost` | `MeetingEndTimeCost` | soft | 1 | per meeting: its last grain index |
| `larger_room_capacity_cost` | `LargerRoomCapacityCost` | soft | 1 | per meeting: capacity difference to every strictly larger room (seats) |
| `room_conflict_cost` | `RoomConflictCost` | hard | 1 | overlapping grains of two meetings in the same room |
| `required_attendance_conflict_cost` | `RequiredAttendanceConflictCost` | hard | 1 | overlapping grains x attendees required at both meetings (person-grains) |
| `required_and_preferred_attendance_conflict_cost` | `RequiredAndPreferredAttendanceConflictCost` | medium | 1 | overlapping grains x attendees required at one and preferred at the other (person-grains) |
| `preferred_attendance_conflict_cost` | `PreferredAttendanceConflictCost` | medium | 1 | overlapping grains x attendees preferred at both (person-grains) |
| `consecutive_meetings_cost` | `ConsecutiveMeetingsCost` | soft | 100 | back-to-back meeting pairs (one ends at the grain the other starts) |
| `meeting_overlap_cost` | `MeetingOverlapCost` | soft | 10 | overlapping grains of any two meetings |
| `room_change_cost` | `RoomChangeCost` | soft | 1 | attendee room switches between meetings at most 2 grains apart |

The first five are unit constraints (one cost per (time grain, room) cell, so `exponent`
shapes per cell); the other seven compare pairs of meetings and are global (one total,
shaped as a whole). The rows are in `list_constraints()` order, and
`meeting_scheduling.constraint_info()` reports exactly this table from the C++ components
(`kDefaultLevel` / `kDefaultWeight`): every constraint is flippable to any of the three
levels (`level="hard"` / `"medium"` / `"soft"`), none is a reward and all twelve are scored
by default (`default_enabled`). The medium level folds into the engine's soft level as
`medium x 1e6` (`MEDIUM_SCALE`, `kMediumFold` in `cpp/include/ms/domain.hpp`); a weight is
in units of the level in effect, so a medium weight is in medium units.

**0.2.0:** the constraints were renamed (`room_conflict` -> `room_conflict_cost`,
`avoid_overtime` -> `overtime_cost`, `required_attendance_conflict` ->
`required_attendance_conflict_cost`, `required_room_capacity` -> `room_capacity_cost`,
`start_and_end_on_same_day` -> `day_crossing_cost`, `required_and_preferred_attendance_conflict`
-> `required_and_preferred_attendance_conflict_cost`, `preferred_attendance_conflict` ->
`preferred_attendance_conflict_cost`, `do_meetings_as_soon_as_possible` ->
`meeting_end_time_cost`, `one_break_between_consecutive_meetings` -> `consecutive_meetings_cost`,
`overlapping_meetings` -> `meeting_overlap_cost`, `assign_larger_rooms_first` ->
`larger_room_capacity_cost`, `room_stability` -> `room_change_cost`; the spec classes and the
module-level default instances follow, e.g. `OverlappingMeetings` / `overlapping_meetings` ->
`MeetingOverlapCost` / `meeting_overlap_cost`). The two medium constraints now default to
weight 1 in medium units (was 1e6 on the soft level), so `level="hard"` charges 1 hard per
person-grain (was 1e6), `level="soft"` demotes them to soft at 1 per person-grain (was a no-op
that kept the 1e6), and an explicit `weight` is in medium units. `compute_breakdown()` and the
`Breakdown` class are gone: `constraint_costs(schedule)` is the per-constraint surface (sum a
constraint's `costs` for the old totals). Raw costs and the default score are unchanged.

**0.3.0:** every constraint takes two more knobs, `offset` and `clip` (keyword-only), and
may be picked several times. A raw cost `x` (per cell for the five unit constraints, the
one total for the seven pair constraints) is shaped as
`sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which charges every
(grain, room) cell of a unit constraint, an empty one included, and acts as a reward below
the offset); `offset=0` is the 0.2.0 shaping. `clip=False` with an offset is refused on
hard (`OvertimeCost`, `RoomConflictCost` and the other hard constraints by default), because
hard 0 must mean no violation; it is allowed on medium, which folds into soft. Each pick is
its own instance with its own knobs, and the instances add up;
`ms.constraint_costs(schedule, picks)` keys the second pick `"<name>#2"`. The CP-SAT
reference takes integral offsets on every constraint (see its README).

The `hard` knob is gone in 0.3.0 (`hard=` raises `TypeError`): `level` replaces it, a
`Level` (`Level.HARD`, `Level.MEDIUM`, `Level.SOFT`, from `meeting_scheduling.constraints`)
or its name. Medium is no longer a default-only level: any constraint may move onto it,
`MeetingOverlapCost(level="medium", weight=3)` charges 3 medium units per overlapping grain
(3e6 soft after the fold). Each repeated pick carries its own level, so one constraint can
count on two levels at once: `MeetingEndTimeCost()` plus
`MeetingEndTimeCost(level="hard", offset=4)` also charges, on the hard level, every meeting
whose last grain index lies beyond 4, by the grains beyond it.

The C++ side builds and tests with:

```sh
cmake -S use-cases/meeting-scheduling/cpp -B build/meeting-scheduling && cmake --build build/meeting-scheduling -j
ctest --test-dir build/meeting-scheduling
```

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose search starts from `solution`, which is the model a solve returned or that model as a JSON dict. It works on the pydantic model, so use `models.demo_data`; `ms.demo_data` is the engine object:

```python
from meeting_scheduling.models import demo_data

problem = demo_data()
solved = ms.solve(problem, time_limit_seconds=5.0, random_seed=42)
seeded = ms.with_initial_solution(problem, solved.model_dump(mode="json"))
result = ms.solve(seeded, time_limit_seconds=5.0, random_seed=43)
print(result.score_hard, result.score_soft)
```

Assignments are matched by id. An assignment the solution leaves out is placed by the construction heuristic, and a complete solution is exactly where the search starts. Pinned assignments keep their value on `problem`, whatever the solution says. `grain_room` is a composite over the positions in `time_grains` and `rooms`, so a solution that lists different ones, or the same ones in another order, raises `ValueError`. So do unknown ids and out-of-range values, before anything reaches the engine, which does not range-check.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/ms/solver.hpp` + `cpp/src/solver.cpp`.

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
`cpp/include/ms/constraints/` (currently `consecutive_meetings_cost`, `day_crossing_cost`,
`larger_room_capacity_cost`, `meeting_end_time_cost`, `meeting_overlap_cost`, `overtime_cost`,
`preferred_attendance_conflict_cost`, `required_and_preferred_attendance_conflict_cost`,
`required_attendance_conflict_cost`, `room_capacity_cost`, `room_change_cost`,
`room_conflict_cost`, plus `common.hpp` with the three shared bases):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint`, usually over one of the shared bases; positive raw cost = penalty,
   0 = no violation) declaring `static constexpr double kDefaultWeight` and
   `static constexpr optimaru::Level kDefaultLevel`, an
   `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `cpp/include/ms/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` (a `Breakdown` member named after the registry name) /
   `incremental_calculator`, weighted by the component's `kDefaultWeight` —
   `ms_plugin_test` checks they agree bit for bit — and a positive and a zero
   hand-calculated case to `ms_test` / `ms_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `python/meeting_scheduling/constraints.py` (plus
   its module-level default instance); callers then pick, reweight or flip it:
   `constraints=[..., MyRuleCost(weight=5, level="hard")]`;
5. mirror it in the CP-SAT model in
   [`benchmarks/meeting-scheduling/cpsat`](../../benchmarks/meeting-scheduling/cpsat) (a plugin
   subclassing the spec; defaults come from `constraint_info()`) so the benchmark stays like
   for like.

## Benchmark

[`benchmarks/meeting-scheduling`](../../benchmarks/meeting-scheduling) compares this use case with OR-Tools CP-SAT on the same instances.

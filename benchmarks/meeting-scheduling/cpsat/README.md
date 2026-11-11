# Meeting scheduling CP-SAT

Full hard/medium/soft CP-SAT formulation for `MeetingScheduleModel`, including
room and attendee conflicts, overtime, capacity, preferences, breaks, room
changes, and pinned assignments. Returned scores come from the engine.

One plugin per engine constraint ([`external_solver.meeting_scheduling.constraints`](https://github.com/optimaru/external-solver/blob/v0.3.0/src/external_solver/meeting_scheduling/constraints.py)), each a
subclass of the domain spec of the same name (`RoomConflictCost`, `OvertimeCost`, ...)
carrying only `build()`; the registry lists them in `list_constraints()` order. The
default level and weight of every plugin come from the engine's
`meeting_scheduling.constraint_info()`, so `catalog()` equals the engine catalog
(external-solver's `tests/meeting_scheduling/test_solve.py`).

- The two medium constraints (`required_and_preferred_attendance_conflict_cost`,
  `preferred_attendance_conflict_cost`) sit on the CP-SAT medium level at weight 1, like
  the engine's medium units. `level="soft"` demotes them to soft and `level="hard"` promotes
  them to hard, one unit per raw person-grain on both solvers. Any other plugin moves onto
  the medium level with `level="medium"` (its weight then in medium units), and each
  repeated pick carries its own level.
- The returned `score_soft` folds medium into soft with the engine's `MEDIUM_SCALE`
  (1e6), so it equals `meeting_scheduling.calculate_score` on the same assignment.
- A constraint may be picked several times (one set of terms per pick, each
  with its own knobs) and takes integral `offset` / `clip` on every constraint: the five
  per-meeting plugins add one term per (grain, room) cell (the engine's unit, an empty
  cell included) and the seven pair plugins add their total once, as the engine shapes
  them (`terms_per_unit`).
- Not supported on CP-SAT (rejected): `exponent != 1`, non-integer weights and fractional
  offsets. `clip=False` with an offset on a hard pick is refused with the engine's message
  when the objective is built; on medium, which folds into soft, it is allowed.

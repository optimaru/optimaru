# Meeting scheduling CP-SAT

Full hard/medium/soft CP-SAT formulation for `MeetingScheduleModel`, including
room and attendee conflicts, overtime, capacity, preferences, breaks, room
changes, and pinned assignments. Returned scores come from the engine.

One plugin per engine constraint (`meeting_scheduling_cpsat/constraints.py`), each a
subclass of the domain spec of the same name (`RoomConflictCost`, `OvertimeCost`, ...)
carrying only `build()`; the registry lists them in `list_constraints()` order. The
default level and weight of every plugin come from the engine's
`meeting_scheduling.constraint_info()` (0.2.0: the hand-copied `default_level` /
`default_weight` ClassVars, e.g. 100 for `consecutive_meetings_cost` and 10 for
`meeting_overlap_cost`, are gone), so `catalog()` equals the engine catalog
(`tests/test_solve.py`).

- The two medium constraints (`required_and_preferred_attendance_conflict_cost`,
  `preferred_attendance_conflict_cost`) sit on the CP-SAT medium level at weight 1, like
  the engine's medium units. `hard=False` demotes them to soft and `hard=True` promotes
  them to hard, one unit per raw person-grain on both solvers.
- The returned `score_soft` folds medium into soft with the engine's `MEDIUM_SCALE`
  (1e6), so it equals `meeting_scheduling.calculate_score` on the same assignment (0.2.0:
  this used 1e9 before, which disagreed with the engine whenever a medium term was
  non-zero).
- Since 0.3.0 a constraint may be picked several times (one set of terms per pick, each
  with its own knobs) and takes integral `offset` / `clip` on every constraint: the five
  per-meeting plugins add one term per (grain, room) cell (the engine's unit, an empty
  cell included) and the seven pair plugins add their total once, as the engine shapes
  them (`terms_per_unit`).
- Not supported on CP-SAT (rejected): `exponent != 1`, non-integer weights and fractional
  offsets.

# School timetabling canonical JSON contract

What this repository defines is the **serialized shape of the problem and the
solution**. The source of truth is the pydantic models
(`TimetableProblem` / `Timetable`) in
`benchmarks/school-timetabling/cpsat/teacher_scheduling/domain/`; this document
pins their JSON shape. `python/` (the pybind11 wrapper) and the benchmark
harness exchange the same shape.

> The HTTP API contract (endpoints, request/response envelopes, storage) is out of scope here.

## Common types

- **Weekday**: integer. `0=Mon 1=Tue 2=Wed 3=Thu 4=Fri 5=Sat`
- **TimeSlot**: `{"day": <Weekday>, "period": <int, 1-based>}`
- pydantic `frozenset` fields serialize to JSON arrays (order not guaranteed).

## TimetableProblem

```json
{
  "calendar": {"periods_per_day": {"0": 7, "1": 7, "2": 7, "3": 7, "4": 6}},
  "subjects": [
    {"id": "MATH", "name": "Math", "required_room_type": null,
     "preferred_periods": [1, 2, 3, 4], "avoided_periods": []},
    {"id": "SCI", "name": "Science", "required_room_type": "science lab",
     "preferred_periods": [], "avoided_periods": []},
    {"id": "PE", "name": "PE", "required_room_type": "gym",
     "preferred_periods": [], "avoided_periods": [1]}
  ],
  "teachers": [
    {"id": "T01", "name": "Kim (Math)", "subject_ids": ["MATH"],
     "unavailable": [{"day": 0, "period": 1}],
     "max_periods_per_week": 20, "max_periods_per_day": 6,
     "max_consecutive_periods": 4, "preferred_free_days": [4],
     "homeroom_class_id": "C11"}
  ],
  "class_groups": [
    {"id": "C11", "name": "Grade 1 Class 1", "grade": 1, "unavailable": []}
  ],
  "rooms": [
    {"id": "R_SCI1", "name": "Science lab 1", "room_type": "science lab"},
    {"id": "R_GYM", "name": "Gym", "room_type": "gym"}
  ],
  "courses": [
    {"id": "C11-MATH", "class_group_id": "C11", "subject_id": "MATH",
     "teacher_ids": ["T01"], "periods_per_week": 4, "block_periods": 0,
     "max_periods_per_day": 1, "fixed_slots": [], "forbidden_slots": [],
     "parallel_group": null}
  ]
}
```

- Two or more `teacher_ids` mean team teaching.
- Courses sharing a `parallel_group` are parallel sections: always at the same time, taking one class cell.
- `block_periods >= 2` joins that many periods into one consecutive block per week.
- `SchoolCalendar` is school-wide. When grades have different periods per weekday, the calendar holds
  the *maximum* per weekday, and shorter grades block the extra cells with their classes' `unavailable`.

## Timetable

```json
{
  "status": "FEASIBLE",
  "lessons": [
    {"course_id": "C11-MATH", "class_group_id": "C11", "subject_id": "MATH",
     "teacher_ids": ["T01"], "slot": {"day": 0, "period": 2}, "room_id": null}
  ],
  "objective_value": 42.0,
  "best_objective_bound": 40.0,
  "wall_time_seconds": 9.7,
  "penalties": {"teacher_gap": 1, "class_gap": 0},
  "notes": []
}
```

- `status`: `OPTIMAL | FEASIBLE | INFEASIBLE | MODEL_INVALID | UNKNOWN`
- `room_id`: filled only for lessons that need a special room. Regular lessons are `null` (their own room).
- With `INFEASIBLE` from the CP-SAT backend (proven), `lessons` is an empty array. The
  local search backend reports `INFEASIBLE` when hard violations are left at the time
  limit; it still returns its best assignment and explains the violations in `notes`.
- There is no hard/soft score field. The quality measures are `status` · `objective_value` ·
  `best_objective_bound` · `penalties`.

## Weights (`ObjectiveWeights`)

The weights of the six soft constraints. `penalties` holds the **raw counts**
under the same keys, and `objective_value` is their weighted sum.

```json
{"teacher_gap": 20, "teacher_workday": 3, "teacher_free_day": 30,
 "class_gap": 10, "avoided_period": 5, "non_preferred_period": 1}
```

## Two backends

`benchmarks/school-timetabling/cpsat` (OR-Tools CP-SAT) and `python` (optimaru
local search) have identical constraint semantics — every hard H1–H10 / soft
S1–S6 constraint and `weights` apply the same way. The parity is gated by
`benchmarks/school-timetabling/cpsat/tests/test_engine_parity.py`.

They differ only in how they search. Local search cannot prove optimality or
infeasibility, so its `status` is `FEASIBLE` (hard 0) or `INFEASIBLE` (hard
violations left at the time limit, not a proof) and
`best_objective_bound` is always `null`.

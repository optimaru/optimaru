# st_timetabling

The Python package wrapping the school-timetabling C++ core (`../cpp`, built on
the engine in `include/optimaru/`) with pybind11. It moves API_SPEC.md's
canonical `TimetableProblem` JSON into the canonical C++ domain (`st::canon`),
solves it, and returns a canonical `Timetable` JSON.

`st::canon` has **full parity** with the CP-SAT reference package
(`teacher_scheduling` in `../../../benchmarks/school-timetabling/cpsat`): it
implements every hard and soft constraint with `validate.py`'s semantics,
under **one name per constraint** in every layer (the C++ component, the spec,
`list_constraints()`, `constraint_info()`, `constraint_costs()`, the CP-SAT
plugin, the `Timetable.penalties` keys and the `weights` keys), and for the
same solution both backends report exactly the same hard-violation count and
weighted soft penalty — [`benchmarks/school-timetabling/cpsat/tests/test_engine_parity.py`](../../../benchmarks/school-timetabling/cpsat/tests/test_engine_parity.py) injects cpsat solutions
into the engine and gates on that parity.

## Constraints

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `unassigned_lessons_cost` | `UnassignedLessonsCost` | hard | 1 | weekly periods without a slot (soft weight to use in partial timetabling: `UNASSIGNED_LESSONS_SOFT_WEIGHT` = 10000) |
| `class_conflict_cost` | `ClassConflictCost` | hard | 1 | pairs of one class's lessons in one period, C(n,2) per (class, period) |
| `teacher_conflict_cost` | `TeacherConflictCost` | hard | 1 | pairs of lessons in one period sharing a teacher, once per shared teacher |
| `disallowed_slot_cost` | `DisallowedSlotCost` | hard | 1 | lessons on a forbidden / unavailable time (engine-only; CP-SAT never creates such a variable) |
| `room_capacity_cost` | `RoomCapacityCost` | hard | 1 | per (special-room type, period): `max(0, demand - rooms)` |
| `course_max_periods_per_day_cost` | `CourseMaxPeriodsPerDayCost` | hard | 1 | per (course, day): lessons over `max_periods_per_day` |
| `block_adjacency_cost` | `BlockAdjacencyCost` | hard | 1 | missing same-day adjacencies of a block, plus one per member left out while the rest is placed |
| `teacher_max_periods_cost` | `TeacherMaxPeriodsCost` | hard | 1 | periods over `max_periods_per_day` per day plus over `max_periods_per_week` per week |
| `teacher_max_consecutive_periods_cost` | `TeacherMaxConsecutivePeriodsCost` | hard | 1 | per (teacher, day): periods of each run beyond `max_consecutive_periods` |
| `teacher_gap_cost` | `TeacherGapCost` | soft | 20 | idle periods inside a teacher's day |
| `teacher_workday_cost` | `TeacherWorkdayCost` | soft | 3 | (teacher, day) pairs with any lesson |
| `teacher_free_day_cost` | `TeacherFreeDayCost` | soft | 30 | worked requested free days (a lesson on a `preferred_free_days` day) |
| `class_gap_cost` | `ClassGapCost` | soft | 10 | idle periods inside a class's day |
| `avoided_period_cost` | `AvoidedPeriodCost` | soft | 5 | lessons in an `avoided_periods` period (once per member course of a parallel group) |
| `non_preferred_period_cost` | `NonPreferredPeriodCost` | soft | 1 | lessons outside a non-empty `preferred_periods` |
| `fixed_slots_cost` | `FixedSlotsCost` | hard, not flippable | 1 | lessons off their fixed slot — always 0: fixed lessons are pinned |
| `parallel_groups_cost` | `ParallelGroupsCost` | hard, not flippable | 1 | sections of a parallel group apart — always 0: a group is one merged lesson |

`st.constraint_info()` reports the same defaults (read off the C++
components' `kDefaultWeight` / `kDefaultLevel`), `st.constraint_costs(table)`
the raw costs of a `canon.Timetable`, and
`st_timetabling.constraints.score(costs, selection)` the engine score of any
selection, in pure Python.

The quickstart model (`mode="quickstart"`) has no constraint registry; its
seven terms are named `room_conflict_cost`, `teacher_conflict_cost`,
`class_conflict_cost`, `teacher_unavailability_cost` (hard),
`teacher_room_change_cost`, `teacher_time_efficiency_cost` (a reward) and
`class_subject_repeat_cost` (soft) in its docs and in the benchmark's
`evaluate.py`.

## Install / test

```bash
cd use-cases/school-timetabling/python
uv sync --all-groups --all-extras   # builds the C++ extension; pytest + pydantic
uv run pytest -q
```

## Usage

```python
import st_timetabling as st

result = st.solve(problem_dict, time_limit_seconds=10.0, random_seed=42)
print(result["status"])  # FEASIBLE (hard 0) | INFEASIBLE (hard violations left)
print(result["penalties"])  # raw soft counts, keyed by the soft constraint names (same keys as cpsat)
print(result["objective_value"])  # weighted penalty sum (same unit as cpsat)
for lesson in result["lessons"]:
    print(lesson["course_id"], lesson["slot"], lesson["room_id"])
```

`weights={"teacher_gap_cost": 40, ...}` changes the soft weights of the
default fast path (`canon.SolverConfig.weights`, a `canon.ObjectiveWeights`
keyed by the soft constraint names; the model itself carries no weights) and
`config=` (the mode's `SolverConfig` or `models.SolveConfig`) the search
options. The low-level API is `convert_problem` /
`canon.Solver(canon.SolverConfig)` / `convert_solution` / `inject_solution`
(injects an external solution, for warm starts and checks); the pydantic
serialization mirrors live in `st_timetabling.models`.

## Plugin constraint assembly (`constraints=`)

The default cost function is the monolithic incremental calculator (fast
path), which scores every constraint above. Passing `constraints=` switches to
the **plugin path** next to it: pick constraints per component, change their
weights, change levels levels and drop some.

```python
import st_timetabling as st
from st_timetabling.constraints import Assembly, ClassConflictCost, TeacherGapCost, get_constraints

print(st.list_constraints())  # the 17 constraint names, in catalog order

result = st.solve(
    problem_dict,
    time_limit_seconds=10.0,
    # Start from the defaults: teacher_gap_cost weight 20 -> 40, teacher_workday_cost dropped.
    constraints=Assembly().add("teacher_gap_cost", weight=40).drop("teacher_workday_cost"),
)
# The same entry point as every other use case: None = the defaults, a list replaces them.
result = st.solve(problem_dict, time_limit_seconds=10.0, constraints=[ClassConflictCost(), TeacherGapCost(weight=40)])
# A constraint may be picked several times: the defaults plus the teacher gaps beyond 3, squared.
result = st.solve(
    problem_dict,
    time_limit_seconds=10.0,
    constraints=[*get_constraints(), TeacherGapCost(weight=1, exponent=2, offset=3)],
)
```

The low-level `canon.DecomposedCalculator` can score and explain directly:

```python
from st_timetabling import canon
from st_timetabling.constraints import ClassConflictCost, TeacherConflictCost, TeacherGapCost

calc = canon.DecomposedCalculator([ClassConflictCost(), TeacherConflictCost(), TeacherGapCost(weight=40)])
calc.add("teacher_max_periods_cost", level="soft", weight=5)  # demote it to a soft penalty
score = calc.score(converted.table)  # score a fixed assignment (stored in table.score)
print(calc.constraint_totals())  # weighted cost per constraint: {name, label, cost, level, is_hard}
```

Rules:

- Every spec and `add` take the five knobs: `weight` (None = the component
  default: 1 for hard, 20/3/30/10/5/1 for the soft ones — never `weights=`,
  which belongs to the fast path and must stay at the defaults next to
  `constraints=`),
  `level` (None keeps the default level, otherwise "hard" or "soft"),
  `exponent` (sign-preserving power shaping) and the keyword-only `offset`
  (`0.0`) / `clip` (`True`): with an offset, `clip=True` counts only what lies
  beyond it (`max(0, c - offset) ** exponent`), `clip=False` shapes `c - offset`
  as it is (every slot of a per-slot constraint is charged). Bad names raise
  `ValueError`, a wrong knob type `TypeError`, in the same words on the Python
  and the C++ path. `fixed_slots_cost` / `parallel_groups_cost` refuse
  `level="soft"`.
- `Assembly.add` **replaces** an existing entry with the same name (override
  semantics: an Assembly holds each name once); a plain list goes through
  `get_constraints`, where a name picked several times is one instance per
  pick, each with its own knobs (`canon.constraint_costs(table, picks)` keys the
  second `"<name>#2"`); `canon.DecomposedCalculator.add` adds another instance
  too. **0.3.0** made both possible (repeats were a `ValueError` before) and
  added `offset` / `clip`; `offset=0` is the 0.2.0 shaping.
- The default assembly matches the monolithic score **bit for bit** (gated by
  `tests/test_constraints.py` and the C++ `canonical_plugin_test`).
- `penalties` in the `solve` response are always the canonical rescore (raw
  soft counts, independent of the assembly); `objective_value` uses the
  assembly's effective soft weights (a picked soft constraint its weight, the
  sum of the instances' weights for one picked several times, one left out 0;
  any non-negative number). Like the exponent, `offset` / `clip` shape the
  search score only; `constraints.score(canon.constraint_costs(table, picks),
  picks)` is the selection's exact score. `status` follows the canonical hard
  rescore too, so an assembly that demotes a hard constraint to soft and ends
  violating it is INFEASIBLE.
- **`unassigned_lessons_cost` is the exception**: demoted to soft, unassigned
  lessons become a **result** instead of a violation (the same contract as
  cpsat's `UnassignedLessonsCost(level="soft")` = `solve_partial`). The search
  gains unassign moves, so "leave it out" becomes an option; unfilled periods
  are reported as `penalties["unassigned_lessons_cost"]`, and `status` comes
  from the hard constraints without that term.

  ```python
  # Leave unfillable periods unassigned instead of INFEASIBLE (partial timetabling).
  st.solve(
      problem_dict,
      constraints=Assembly().add("unassigned_lessons_cost", level="soft", weight=st.UNASSIGNED_LESSONS_SOFT_WEIGHT),
  )
  ```

  One unassigned period costs exactly that `weight`. `UNASSIGNED_LESSONS_SOFT_WEIGHT`
  (10000, C++ `UnassignedLessonsCost::kSoftWeight`) is the documented soft
  weight — above the sum of the other soft terms; `weight=None` would keep the
  component default 1, cheap enough to drop a lesson to save one gap. Do not
  use `stop_at_first_feasible` with this assembly: a solution that drops
  everything is hard 0 too and the search would stop there.
- The plugin path is single-start (`parallel_solver_count` is ignored; a
  caller-owned calculator cannot be cloned). When performance comes first, keep
  the default fast path.
- Thanks to structural optimisations (compiled delta groups for the per-slot
  family, shared projections for the teacher-mask family), the plugin path's
  move throughput is back to 35–55% of the monolith (track 2 S/M/L at 10 s;
  the rest of the gap is the fixed cost of the generic assembly dispatch). This
  holds for partial assemblies and weight overrides too.

## 0.2.0: one name per constraint

- Every constraint has one public name, `<subject>_cost`, in every layer. The
  old engine names (`unassigned_lesson`, `class_conflict`, `teacher_conflict`,
  `slot_not_allowed`, `room_capacity`, `course_daily_limit`, `block_adjacency`,
  `teacher_load_limit`, `teacher_consecutive_limit`, `teacher_gap`,
  `teacher_workday`, `teacher_free_day`, `class_gap`, `avoided_period`,
  `non_preferred_period`), the CP-SAT / server spellings bridged by the removed
  `CPSAT_ALIASES` (`weekly_period_counts`, `class_group_conflicts`,
  `teacher_conflicts`, `daily_course_limits`, `block_periods`,
  `teacher_workload`, `teacher_consecutive_limits`) and the structural
  `fixed_slots` / `parallel_groups` are unknown now. `CPSAT_ALIASES`,
  `STRUCTURAL_IN_ENGINE`, `ENGINE_ONLY` and `resolve_name` are gone.
- `fixed_slots_cost` and `parallel_groups_cost` are real components (always 0,
  hard, not flippable), so `list_constraints()` holds 17 names.
- The C++ registry names are the struct names (`teacherGapCost`); the H/S
  codes and display strings (`"S1 teacher gap"`) are gone, also from
  `constraint_totals()`.
- The canonical model carries no weights: `canon.Timetable.weights` is
  removed; `weight=None` is the component default everywhere. The fast path's
  `canon.ObjectiveWeights` / `SolverConfig.weights` / `solve(weights=)` /
  `models.ObjectiveWeightsModel` are keyed by the soft constraint names
  (`teacher_gap_cost`, ...) and take any finite number >= 0 (a bool, NaN,
  infinity or a negative weight raises, as for a constraint pick's weight).
- `Timetable.penalties` keys are the soft constraint names; partial
  timetabling reports `unassigned_lessons_cost` (was `weekly_period_counts`).
- `canon.Breakdown` and `canon.compute_breakdown` are removed:
  `canon.constraint_costs(table)` is the per-constraint surface.
  `canon.default_calculator()` takes no weights.

## Canonical conversion (default mode)

- Each course becomes `periods_per_week` engine Lessons (id `"{course}#k"`).
- A parallel group (`parallel_group`) folds into **one merged lesson** per
  weekly period: teacher union, summed special-room demand, class occupancy 1
  (the same as cpsat's `occupancy_key`). The sections running together
  (`parallel_groups_cost`) holds by the merge itself, and each member
  subject's preferred/avoided periods count separately through
  `preference_units`, so `avoided_period_cost` / `non_preferred_period_cost`
  match exactly too. The reverse conversion unfolds it into one canonical lesson per member
  course.
- `block_periods` >= 2 gives that many of the course's lessons a blockGroup,
  enforcing consecutive periods on one day (`block_adjacency_cost`, hard).
- `fixed_slots` -> pinned lessons (`fixed_slots_cost`: the engine never moves
  them); `forbidden_slots` + class/teacher `unavailable` -> a per-lesson
  allowed_slot table (`disallowed_slot_cost`, hard).
- With team teaching every teacher goes into `teachers`, so clashes, caps and
  gaps are checked for each.
- `required_room_type` is a hard constraint (`room_capacity_cost`) that
  demand <= rooms per (type, time); concrete rooms are assigned afterwards by a greedy pass, as in
  cpsat — it always succeeds on a hard-0 solution.
- Per-teacher period caps (`teacher_max_periods_cost`,
  `teacher_max_consecutive_periods_cost`), `preferred_free_days`
  (`teacher_free_day_cost`) and the per-course daily cap
  (`course_max_periods_per_day_cost`) are passed through.

## Remaining limits

With full parity, what remains are properties of the search and the design, not of expressiveness:

- Local search **cannot prove** optimality or infeasibility: status is only
  FEASIBLE (hard 0) or INFEASIBLE (hard violations left at the time limit —
  not a proof), and `best_objective_bound` is always null (use the cpsat
  backend when you need a proof).
- Bitmask design limits: periods <= 16, weekday index < 8 (a larger calendar
  is rejected with ValueError).
- A solution with hard violations left (INFEASIBLE) may report unassigned lessons
  or missing special rooms in `notes`. On a hard-0 solution notes are always
  empty.

## Quickstart mode (`mode="quickstart"`)

The approximate conversion to the quickstart's 7-term domain (slotRoom
composite encoding) remains in `st_timetabling.quickstart`. It cannot express
blocks, parallel sections, forbidden times, period caps, preferred periods and
the like, and its soft objective is replaced by the engine's built-in
constraints — every limit is stated in the response `notes`. See the
quickstart module docstring for details.

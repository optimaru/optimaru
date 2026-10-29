# st_timetabling

The Python package wrapping the school-timetabling C++ core (`../cpp`, built on
the engine in `include/optimaru/`) with pybind11. It moves API_SPEC.md's
canonical `TimetableProblem` JSON into the canonical C++ domain (`st::canon`),
solves it, and returns a canonical `Timetable` JSON.

`st::canon` has **full parity** with the CP-SAT reference package
(`teacher_scheduling` in `../../../benchmarks/school-timetabling/cpsat`): it
implements every hard H1–H10 / soft S1–S6 constraint with `validate.py`'s
semantics, and for the same solution both backends report exactly the same
hard-violation count and weighted soft penalty — [`benchmarks/school-timetabling/cpsat/tests/test_engine_parity.py`](../../../benchmarks/school-timetabling/cpsat/tests/test_engine_parity.py) injects cpsat solutions
into the engine and gates on that parity.

## Install / test

```bash
cd use-cases/school-timetabling/python
uv venv
uv pip install -e .
uv sync --all-groups   # pytest + pydantic
uv run pytest -q
```

## Usage

```python
import st_timetabling as st

result = st.solve(problem_dict, time_limit_seconds=10.0, random_seed=42)
print(result["status"])  # FEASIBLE (hard 0) | INFEASIBLE (hard violations left)
print(result["penalties"])  # raw S1–S6 counts (same keys as cpsat)
print(result["objective_value"])  # weighted penalty sum (same unit as cpsat)
for lesson in result["lessons"]:
    print(lesson["course_id"], lesson["slot"], lesson["room_id"])
```

`weights=dict(...)` changes the soft weights and `config=` (the mode's
`SolverConfig` or `models.SolveConfig`) the search options. The low-level API
is `convert_problem` / `canon.Solver(canon.SolverConfig)` / `convert_solution` /
`inject_solution` (injects an external solution, for warm starts and checks);
the pydantic serialization mirrors live in `st_timetabling.models`.

## Plugin constraint assembly (`constraints=`)

The default cost function is the monolithic incremental calculator (fast
path), which scores all of H1–H10 / S1–S6. Passing `constraints=` switches to
the **plugin path** next to it: declare an assembly that picks constraints per
component, changes their weights, flips hard/soft levels and drops some.

```python
import st_timetabling as st
from st_timetabling.constraints import Assembly, list_constraints

print(list_constraints())  # the 15 assemblable constraint names (H1, H3–H10, S1–S6)

result = st.solve(
    problem_dict,
    time_limit_seconds=10.0,
    # Start from the default 15: S1 weight 20 -> 40, S2 dropped altogether.
    constraints=Assembly().add("teacher_gap", weight=40).drop("teacher_workday"),
)
```

An assembly can also be built from scratch as a list of spec instances, or
the low-level `canon.DecomposedCalculator` can score and explain directly:

```python
from st_timetabling import canon
from st_timetabling.constraints import ClassConflict, TeacherConflict, TeacherGap

calc = canon.DecomposedCalculator([ClassConflict(), TeacherConflict(), TeacherGap(40)])
calc.add("teacher_load_limit", hard=False, weight=5)  # demote H9 to a soft penalty
score = calc.score(converted.table)  # score a fixed assignment (stored in table.score)
print(calc.constraint_totals())  # weighted cost per constraint (the score explanation)
```

Rules:

- Every spec and `add` take `weight` (None = the component default: 1 for
  hard, the cpsat default weight for soft — in `solve` the `weights=` argument
  replaces that default), `hard` (True/False flips the default level) and
  `exponent` (sign-preserving power shaping). Bad names raise `ValueError`.
- `Assembly.add` **replaces** an existing entry with the same name (override
  semantics); for summed duplicates use `canon.DecomposedCalculator.add`.
- The default assembly (`Assembly()`) with the default weights matches the
  monolithic score **bit for bit** (gated by `tests/test_constraints.py`).
- `penalties` in the `solve` response are always the canonical rescore (raw
  S1–S6 counts, independent of the assembly); `objective_value` uses the
  assembly's effective S weights (an override takes its value, a dropped term
  0) — hence S weights must be non-negative integers. `status` follows the
  canonical hard rescore too, so an assembly that demotes a hard constraint to
  soft and ends violating it is INFEASIBLE.
- **H1 (`unassigned_lesson`) is the exception**: demoted to soft, unassigned
  lessons become a **result** instead of a violation (the same contract as
  cpsat's `WeeklyPeriodCounts(hard=false)` = `solve_partial`). The search gains
  unassign moves, so "leave it out" becomes an option; unfilled periods are
  reported as `penalties["weekly_period_counts"]`, and `status` comes from the
  hard constraints without that term.

  ```python
  # Leave unfillable periods unassigned instead of INFEASIBLE (partial timetabling).
  st.solve(problem_dict, constraints=Assembly().add("weekly_period_counts", hard=False, weight=10_000))
  ```

  One unassigned period costs exactly that `weight` — keep it above the sum of
  the other soft terms (cpsat default 10000). Do not use `stop_at_first_feasible`
  with this assembly: a solution that drops everything is hard 0 too and the
  search would stop there.
- The plugin path is single-start (`parallel_solver_count` is ignored; a
  caller-owned calculator cannot be cloned). When performance comes first, keep
  the default fast path.
- Thanks to structural optimisations (compiled delta groups for the slot
  family H3/H4/H5/H6/S5/S6, shared projections for the teacher-mask family
  H9/H10/S1–S3), the plugin path's move throughput is back to 35–55% of the
  monolith (track 2 S/M/L at 10 s; the rest of the gap is the fixed cost of the
  generic assembly dispatch). This holds for partial assemblies and weight
  overrides too.

## Canonical conversion (default mode)

- Each course becomes `periods_per_week` engine Lessons (id `"{course}#k"`).
- A parallel group (`parallel_group`) folds into **one merged lesson** per
  weekly period: teacher union, summed special-room demand, class occupancy 1
  (the same as cpsat's `occupancy_key`). The sections running together (H5)
  holds by the merge itself, and each member subject's preferred/avoided
  periods count separately through `preference_units`, so S5/S6 match exactly
  too. The reverse conversion unfolds it into one canonical lesson per member
  course.
- `block_periods` >= 2 gives that many of the course's lessons a blockGroup,
  enforcing consecutive periods on one day (H8, hard).
- `fixed_slots` -> pinned lessons (H2: the engine never moves them);
  `forbidden_slots` + class/teacher `unavailable` -> a per-lesson allowed_slot
  table (H5, hard).
- With team teaching every teacher goes into `teachers`, so clashes, caps and
  gaps are checked for each.
- `required_room_type` is a hard constraint (H6) that demand <= rooms per
  (type, time); concrete rooms are assigned afterwards by a greedy pass, as in
  cpsat — it always succeeds on a hard-0 solution.
- Per-teacher period caps (H9/H10), `preferred_free_days` (S3), the per-course
  daily cap (H7) and all of `ObjectiveWeights` are passed through.

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

The approximate conversion to the quickstart's 6-constraint domain (slotRoom
composite encoding) remains in `st_timetabling.quickstart`. It cannot express
blocks, parallel sections, forbidden times, period caps, preferred periods and
the like, and its soft objective is replaced by the engine's built-in
constraints — every limit is stated in the response `notes`. See the
quickstart module docstring for details.

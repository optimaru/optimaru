# School timetabling benchmark

Transcribes the Timefold Quickstarts `school-timetabling` example's constraint scoring (Apache-2.0),
with modifications; see the repository [NOTICE](../../NOTICE).

A harness that solves the same instances with the engine and OR-Tools CP-SAT
and compares them. It runs build → cross-check gate → quality benchmark → table.
It is the package `school_timetabling_cpsat` of the `uv` project [`cpsat/`](cpsat), next to
the CP-SAT reference `teacher_scheduling`, and runs from that directory:

```sh
cd benchmarks/school-timetabling/cpsat
OPTIMARU_USE_CASES=school_timetabling uv sync   # also builds this checkout's optimaru (school_timetabling only)
uv run pytest -q                                # the CP-SAT model's tests and its parity with the engine
uv run school-timetabling-compare --help
```

The helper modules also run on their own, as
`uv run python -m school_timetabling_cpsat.<module>` (`evaluate`, `cpsat_quickstart`,
`gen_canonical`). The runner and the generator write to `out/` in the working directory
(`--out-dir`).

| File (`cpsat/school_timetabling_cpsat/`) | Role |
| --- | --- |
| `benchmark.py` | `school-timetabling-compare`: orchestration (track 1 by default, `--track2` for the canonical track, `--track3` for partial timetabling) |
| `evaluate.py` | Track-1 shared evaluator: recomputes the quickstart model's six-constraint score from a solution (checked against each solver's self-report); its per-term keys are the `*_cost` names below, each holding the raw cost (the reward `teacher_time_efficiency_cost` is <= 0) |
| `cpsat_quickstart.py` | Track-1 CP-SAT solver (six constraints: hard as constraints, the three soft ones as the objective) |
| `gen_canonical.py` | Track-2 generator of S/M/L canonical `TimetableProblem`s (blocks / parallel sections / team teaching / unavailability, with a load check) |
| `../cpp/st_quality_bench.cpp` | Track-1 C++ side: builds instances, dumps JSON, solves, cross-check mode |

## Track 1: the quickstart model's six constraints

The shared logic is the six constraints of the quickstart model (`mode="quickstart"`)
(`room_conflict_cost` / `teacher_conflict_cost` / `class_conflict_cost` hard,
`teacher_room_change_cost` / `teacher_time_efficiency_cost` (a reward) /
`class_subject_repeat_cost` soft). The model's seventh term, `teacher_unavailability_cost`,
gets empty data (`teacherUnavailable`) in these
instances, so it contributes 0.

Size presets (classes / teachers / rooms — same as `st_quality_bench.cpp`):
`S: 6/12/7 (204 lessons)`, `M: 12/20/13 (408 lessons)`, `L: 24/40/25 (816 lessons)`.
The grid is 5 days × 8 periods = 40 slots.

```sh
# cross-check gate → S/M 10 s each → rescoring → Markdown table
uv run school-timetabling-compare --sizes S,M --seconds 10
```

The result is `out/RESULTS_track1.md`. The cross-check gate passes only when
the C++ score of the fixed instance matches `evaluate.py`
(`CROSSCHECK_SCORE=-3hard/-3soft`).

## Track 2: full canonical constraints, C++ canonical vs CP-SAT

The canonical `TimetableProblem` (every hard and soft canonical constraint) is solved under the same
time limit by CP-SAT (`teacher_scheduling.solve`) and the C++ canonical
engine (`optimaru.school_timetabling.solve`, single-threaded — shown as `pending` when the
extension is not built and the import fails); both results are rescored with
cpsat's `validate.py`.

```sh
uv run school-timetabling-compare --track2 --sizes S,M,L --seconds 10
```

The result is `out/RESULTS_track2.md` (status / hard-violation count / penalty
at the default weights).

## Track 3: partial timetabling (unassigned lessons allowed)

A track-2 instance loses one weekday (`--drop-day`, default 4 = Friday) so
that **periods > cells**, and both sides solve with `unassigned_lessons_cost`
(weekly periods) relaxed — CP-SAT with `solve_partial`, the engine with the
equivalent assembly (`unassigned_lessons_cost level=soft weight=10000`, the
documented soft weight `UNASSIGNED_LESSONS_SOFT_WEIGHT`). Placing everything is
impossible from the start, so the comparison is **how much gets filled**.

```sh
uv run school-timetabling-compare --track3 --sizes S,M --seconds 10
```

The result is `out/RESULTS_track3.md` (placed/required · unassigned · hard
left · weighted penalty).

How to read it:

- **placed/required** is the score. Placing less also lowers the weighted
  penalty, so read the two together.
- **hard left** are the hard violations besides unassigned lessons. Even a
  partial solution must respect every hard constraint on what it places, so
  anything above 0 is simply broken — this column is the track's consistency
  gate.
- Time references (teacher/class unavailability, fixed and forbidden
  placements) pointing at the removed weekday are removed too, because input
  validation rejects times that are not in the calendar.

## Fairness notes

- `st_quality_bench` accepts environment overrides for tuning sweeps
  (`ST_BENCH_ASMOVE_WEIGHT` / `ST_BENCH_ASMOVE_MAXRUIN` /
  `ST_BENCH_NEARBY_PROB` / `ST_BENCH_LA_SIZE`). **Unset, the solver defaults
  apply** — CI and the published tables always run without them. They change
  search knobs only, never the scoring semantics.
- Both solvers get the same instance JSON and the same wall-clock budget.
- C++ runs single-threaded, CP-SAT as an 8-worker portfolio by default (`--workers`).
- moves/s: C++ is estimated from the improvement trajectory (one move per
  step); CP-SAT has none.
- Every solution enters the table through the shared evaluator's rescoring
  (`evaluate.py`, or `validate.py` for track 2), never through its self-reported score.

## CP-SAT reference (`cpsat/`, `teacher_scheduling`)

Its constraint registry (`teacher_scheduling/solver/registry.py`) subclasses the
engine's specs (`optimaru.school_timetabling.constraints`): the same 17 names, the same
`(weight=None, level=None, exponent=1.0, *, offset=0.0, clip=True)` knobs and the engine's defaults from
`optimaru.school_timetabling.constraint_info()` — nothing is restated. The hard constraints
are CP-SAT model constraints, so on this path:

- `unassigned_lessons_cost` with `level="soft"` is partial timetabling (`solve_partial`).
  `weight=None` there is the engine default, 1, so a lesson would be dropped to save a single
  teacher gap (20): pass `weight=UNASSIGNED_WEIGHT` (10000, the engine's
  `UNASSIGNED_LESSONS_SOFT_WEIGHT`) for partial timetabling, as `solve_partial` does;
- every other hard constraint refuses `level="soft"`, and a weight 0 leaves it out.
  `fixed_slots_cost` / `parallel_groups_cost` are not flippable in the engine either; the
  other eight (`class_conflict_cost` ... `teacher_max_consecutive_periods_cost`) are model
  constraints here without a penalty form, so they are structural on CP-SAT only, although the
  catalog (read from the engine) lists them as `kind="either"`;
- a soft constraint with `level="hard"` forbids every violation (`model.add(expr == 0)`);
  `level="medium"` is refused, as by the engine (this use case has no medium level);
- weights must be integers and `exponent` must be 1 (a linear CP-SAT objective);
- an integral `offset` / `clip` is shaped as the engine shapes it on a soft constraint: the
  one total of a global constraint, or each calendar slot of `avoided_period_cost` /
  `non_preferred_period_cost` (an empty slot included); a hard constraint (a model
  constraint here) and a fractional offset refuse a non-zero offset, and `clip=False` with
  an offset on hard is refused first, with the engine's message (hard 0 must mean no
  violation);
- a constraint listed several times in `solve(constraints=...)` adds one objective term per
  entry, each with its own knobs, its level included (`avoided_period_cost` once hard
  and once soft forbids the avoided periods and still prices its soft repeat);
  `Timetable.penalties` reports each name's raw count once, and `registry.merge` keeps its
  replace-by-name semantics, like the engine's `Assembly`.

`Timetable.penalties`, `validate.compute_penalties` and `SolverConfig.weights` are keyed by
the soft constraint names (`teacher_gap_cost`, ...), the same keys as the engine.

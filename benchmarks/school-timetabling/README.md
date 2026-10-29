# School timetabling benchmark

A harness that solves the same instances with the engine and OR-Tools CP-SAT
and compares them. It runs build → cross-check gate → quality benchmark → table.

| File | Role |
| --- | --- |
| `run_bench.py` | Orchestration (track 1 by default, `--track2` for the canonical track) |
| `evaluate.py` | Track-1 shared evaluator: recomputes the quickstart's six-constraint score from a solution (checked against each solver's self-report) |
| `cpsat_quickstart.py` | Track-1 CP-SAT solver (six constraints: hard as constraints, the three soft ones as the objective) |
| `gen_canonical.py` | Track-2 generator of S/M/L canonical `TimetableProblem`s (blocks / parallel sections / team teaching / unavailability, with a load check) |
| `../../use-cases/school-timetabling/cpp/bench/st_quality_bench.cpp` | Track-1 C++ side: builds instances, dumps JSON, solves, cross-check mode |

## Track 1: the quickstart's six constraints

The shared logic is the original Java quickstart's six constraints
(roomConflict / teacherConflict / studentGroupConflict hard,
teacherRoomStability / teacherTimeEfficiency / studentGroupSubjectVariety
soft). The teacherUnavailable extension of this repository's C++ domain gets
empty data in these instances, so it contributes 0.

Size presets (classes / teachers / rooms — same as `st_quality_bench.cpp`):
`S: 6/12/7 (204 lessons)`, `M: 12/20/13 (408 lessons)`, `L: 24/40/25 (816 lessons)`.
The grid is 5 days × 8 periods = 40 slots.

```sh
# cross-check gate → S/M 10 s each → rescoring → Markdown table
uv run run_bench.py --sizes S,M --seconds 10
```

The result is `out/RESULTS_track1.md`. The cross-check gate passes only when
the C++ score of the fixed instance matches `evaluate.py`
(`CROSSCHECK_SCORE=-3hard/-3soft`).

## Track 2: full canonical constraints, C++ canonical vs CP-SAT

The canonical `TimetableProblem` (H1–H10 + S1–S6) is solved under the same
time limit by CP-SAT (`teacher_scheduling.solve`) and the C++ canonical
engine (`st_timetabling.solve`, single-threaded — shown as `pending` when the
extension is not built and the import fails); both results are rescored with
cpsat's `validate.py`.

```sh
uv run run_bench.py --track2 --sizes S,M,L --seconds 10
```

The result is `out/RESULTS_track2.md` (status / hard-violation count / penalty
at the default weights).

## Track 3: partial timetabling (unassigned lessons allowed)

A track-2 instance loses one weekday (`--drop-day`, default 4 = Friday) so
that **periods > cells**, and both sides solve with H1 (weekly periods)
relaxed — CP-SAT with `solve_partial`, the engine with the equivalent assembly
(`weekly_period_counts hard=false weight=10000`). Placing everything is
impossible from the start, so the comparison is **how much gets filled**.

```sh
uv run run_bench.py --track3 --sizes S,M --seconds 10
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

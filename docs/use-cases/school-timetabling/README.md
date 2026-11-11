# School timetabling

Derived from the Timefold Quickstarts `school-timetabling` example (Apache-2.0), translated
and modified; see the repository [NOTICE](../../../NOTICE).

Domain model, constraints and API details: [`python.md`](python.md).

## Usage

```sh
OPTIMARU_USE_CASES=school_timetabling uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/school_timetabling
```

```python
import json

problem = json.load(open("problem.json"))   # canonical TimetableProblem, see API_SPEC.md
from optimaru import school_timetabling as st

timetable = st.solve(problem, time_limit_seconds=5.0, random_seed=42)  # canonical Timetable dict
print(timetable["status"], len(timetable["lessons"]))
```

`solve` takes `mode` (`"canonical"`, the default, or `"quickstart"`), `time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`, `full_assert`, `weights`, `on_new_best`, a full `config` (`optimaru.school_timetabling.canon.SolverConfig` in the default mode, `optimaru.school_timetabling.SolverConfig` in quickstart mode, or a `models.SolveConfig`), `initial_solution` (below) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -B build -DOPTIMARU_USE_CASES=school_timetabling && cmake --build build -j
ctest --test-dir build
```

## Constraints

The constraint table (names, default levels and weights) is in
[`python.md`](python.md#constraints); it is not repeated here.

In a `constraints=` list a constraint may be picked several times (each pick its own
instance with its own knobs; the instances add up), and besides `weight`, `level` and
`exponent` every constraint takes the keyword knobs `offset` and `clip`: `TeacherGapCost(exponent=2, offset=3)` charges
`max(0, gaps - 3)**2`, `clip=False` shapes `gaps - 3` as it is (refused with an offset on
hard, the structural `fixed_slots_cost` / `parallel_groups_cost` included, because hard 0
must mean no violation). `offset=0` is the plain shaping. `constraints.Assembly` stays a
builder that holds each name once (`add` replaces).
`level` is `"hard"` / `"soft"` or `Level.HARD` / `Level.SOFT`, each repeated pick with its own, and
`level="medium"` raises `ValueError` (this use case has no medium level).
Details: [`optimaru/school_timetabling/`](python.md#plugin-constraint-assembly-constraints).

## Starting from an existing solution

```python
timetable = st.solve(problem, time_limit_seconds=5.0, random_seed=42)
better = st.solve(problem, time_limit_seconds=5.0, random_seed=42, initial_solution=timetable)
print(better["status"], better["objective_value"])
```

Problems here are dicts, so `solve` takes the earlier `Timetable` dict directly in `initial_solution`, in both modes. Lessons are matched by course id, and construction places only the periods the solution leaves out, so a partial solution works. With `time_limit_seconds=0` a complete solution comes back as it went in. `room_id` is ignored, since it is recomputed from the result.

`fixed_slots` stay pinned, so a course's slots beyond them must fit its free periods. An unknown course, a course given twice at one time, more slots than a course has free periods, or parallel sections at different times raise `ValueError`. A time a course may not use is accepted and scores as a hard violation, so an `INFEASIBLE` result can be resumed.

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **assignment** shape; the domain solver that configures the
engine is `optimaru/school_timetabling/cpp/include/st/canonical_solver.hpp` + `optimaru/school_timetabling/cpp/src/canonical_solver.cpp` (the default canonical model; `solver.hpp` is the six-constraint quickstart model).

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`). `SmartClassSwapMove`, `ReassignMove` and `UnassignMove` live in `optimaru/school_timetabling/cpp/include/st/moves/` and are registered in `optimaru/school_timetabling/cpp/src/canonical_solver.cpp` — add a new move as another file there. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.add_move_operator(...)` next
to the engine construction in `optimaru/school_timetabling/cpp/src/canonical_solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** `difficulty` is already set in `optimaru/school_timetabling/cpp/src/canonical_solver.cpp`. Set
`engineConfig.construction.difficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** [Adding a constraint, step by step](extending.md) takes one new
constraint through this use case's files in six steps: changed from Python, written as a
component, tested, made cheaper per move, moved to the default fast path and measured.

## Benchmark

[`benchmarks/school-timetabling`](../../../benchmarks/school-timetabling) compares this use case with
OR-Tools CP-SAT on the same instances, rescored by the same evaluator.

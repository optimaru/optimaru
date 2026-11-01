# School Timetabling

Domain model, constraints and API details: [`python/`](python/README.md).

## Usage

```sh
cd use-cases/school-timetabling/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import json

problem = json.load(open("problem.json"))   # canonical TimetableProblem, see API_SPEC.md
import st_timetabling as st

timetable = st.solve(problem, time_limit_seconds=5.0, random_seed=42)  # canonical Timetable dict
print(timetable["status"], len(timetable["lessons"]))
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`st_timetabling.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/school-timetabling/cpp -B build/school-timetabling && cmake --build build/school-timetabling -j
ctest --test-dir build/school-timetabling
```

**0.3.0:** in a `constraints=` list a constraint may be picked several times (each pick
its own instance with its own knobs; the instances add up), and every constraint takes two
more keyword knobs, `offset` and `clip`: `TeacherGapCost(exponent=2, offset=3)` charges
`max(0, gaps - 3)**2`, `clip=False` shapes `gaps - 3` as it is. `offset=0` is the 0.2.0
shaping. `constraints.Assembly` stays a builder that holds each name once (`add` replaces).
Details: [`python/`](python/README.md#plugin-constraint-assembly-constraints).

## Starting from an existing solution

```python
timetable = st.solve(problem, time_limit_seconds=5.0, random_seed=42)
better = st.solve(problem, time_limit_seconds=5.0, random_seed=42, initial_solution=timetable)
print(better["status"], better["objective_value"])
```

Problems here are dicts, so `solve` takes the earlier `Timetable` dict directly in `initial_solution`, in both modes. Lessons are matched by course id, and construction places only the periods the solution leaves out, so a partial solution works. With `time_limit_seconds=0` a complete solution comes back as it went in. `room_id` is ignored, since it is recomputed from the result.

`fixed_slots` stay pinned, so a course's slots beyond them must fit its free periods. An unknown course, a course given twice at one time, more slots than a course has free periods, or parallel sections at different times raise `ValueError`. A time a course may not use is accepted and scores as a hard violation, so an `INFEASIBLE` result can be resumed.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/st/canonical_solver.hpp` + `cpp/src/canonical_solver.cpp` (the default canonical model; `solver.hpp` is the six-constraint quickstart model).

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`). `SmartClassSwapMove`, `ReassignMove` and `UnassignMove` live in `cpp/include/st/moves/` and are registered in `cpp/src/canonical_solver.cpp` — add a new move as another file there. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` next
to the engine construction in `cpp/src/canonical_solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** `entityDifficulty` is already set in `cpp/src/canonical_solver.cpp`. Set
`engineConfig.constructionHeuristic.entityDifficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/st/constraints/` (currently `class_gap_cost`, `global/block_adjacency_cost`,
`global/course_max_periods_per_day_cost`, `global/fixed_slots_cost`,
`global/parallel_groups_cost`, `global/unassigned_lessons_cost`, `slot/avoided_period_cost`,
`slot/class_conflict_cost`, `slot/disallowed_slot_cost`, `slot/non_preferred_period_cost`,
`slot/room_capacity_cost`, `slot/teacher_conflict_cost`, `teacher/teacher_free_day_cost`,
`teacher/teacher_gap_cost`, `teacher/teacher_max_consecutive_periods_cost`,
`teacher/teacher_max_periods_cost`, `teacher/teacher_workday_cost`; the helper bases
`global/global_constraint.hpp`, `slot/slot_constraint.hpp`, `slot/slot_delta_group.hpp`,
`teacher/teacher_mask_hub.hpp`):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint`; positive raw cost = penalty, 0 = no violation) declaring
   `static constexpr double kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
   an `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `cpp/include/st/canonical_constraints.hpp` and add it to
   `detail::buildDefaultSet()` (whose order is the catalog order) if it belongs to the
   default set;
3. for a default constraint, add the same raw cost to `canonical_score_calculator`
   (a `Breakdown` member named like the registry name) / `canonical_incremental_calculator`,
   weighted by the component's `kDefaultWeight` (a soft one also gets an `ObjectiveWeights`
   field) — `canonical_plugin_test` checks they agree bit for bit — and a positive and a zero
   hand-calculated case to `canonical_plugin_test` (h);
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `python/st_timetabling/constraints.py` (plus its
   module-level default instance and its place in `_DEFAULTS`); callers then pick, reweight
   or flip it: `constraints=get_constraints([..., MyRuleCost(weight=5, level="hard")])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/school-timetabling/cpsat`](../../benchmarks/school-timetabling/cpsat) (a
   plugin subclassing the spec in `teacher_scheduling/solver/registry.py`; defaults come from
   `constraint_info()`) so the benchmark stays like for like.

The constraint table (names, defaults, raw costs) and the 0.2.0 renames are in
[`python/README.md`](python/README.md#constraints).

## Benchmark

[`benchmarks/school-timetabling`](../../benchmarks/school-timetabling) compares this use case with
OR-Tools CP-SAT on the same instances, rescored by the same evaluator.

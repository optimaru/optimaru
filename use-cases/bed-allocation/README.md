# Bed Allocation

Domain model, constraints and API details: [`python/`](python/README.md).

## Usage

```sh
cd use-cases/bed-allocation/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import bed_allocation as ba
from bed_allocation.demo import generate_demo_data

plan = generate_demo_data()                       # models.BedPlanModel
result = ba.solve(plan, time_limit_seconds=5.0, random_seed=42)
print(result.score_hard, result.score_medium, result.score_soft)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`bed_allocation.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/bed-allocation/cpp -B build/bed-allocation && cmake --build build/bed-allocation -j
ctest --test-dir build/bed-allocation
```

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose search starts from `solution`, which is the model a solve returned or that model as a JSON dict:

```python
solved = ba.solve(plan, time_limit_seconds=5.0, random_seed=42)
seeded = ba.with_initial_solution(plan, solved.model_dump(mode="json"))
result = ba.solve(seeded, time_limit_seconds=5.0, random_seed=43)
print(result.score_hard, result.score_medium, result.score_soft)
```

Stays are matched by id. A stay the solution leaves out is placed by the construction heuristic, and a complete solution is exactly where the search starts. A stay with no bed has no bed id to carry, so it comes back unassigned and construction places it again. Unknown ids raise `ValueError` before anything reaches the engine, which does not range-check.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/ba/solver.hpp` + `cpp/src/solver.cpp`.

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
`cpp/include/ba/constraints/` (currently `assign_every_patient_to_a_bed`, `department_maximum_age`, `department_minimum_age`, `department_specialty`, `department_specialty_not_first_priority`, `different_gender_in_same_gender_room_in_same_night`, `female_in_male_room`, `male_in_female_room`, `preferred_maximum_room_capacity`, `preferred_patient_equipment`, `required_patient_equipment`, `same_bed_in_same_night`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/ba/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `ba_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/bed_allocation/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/bed-allocation/cpsat`](../../benchmarks/bed-allocation/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/bed-allocation`](../../benchmarks/bed-allocation) compares this use case with OR-Tools CP-SAT on the same instances.

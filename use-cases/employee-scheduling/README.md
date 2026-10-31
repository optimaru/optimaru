# Employee Scheduling

Domain model, constraints and API details: [`python/`](python/README.md).

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/employee-scheduling/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
from es_scheduling import with_initial_solution
from es_scheduling.demo import generate_demo_data
from es_scheduling.models import solve_request

request = generate_demo_data()                    # models.ScheduleRequest
request.timelimit = 2.0
solution = solve_request(request)
print(solution.score_hard, solution.score_soft)

# Start again from that result: construction only fills what it leaves out.
request = with_initial_solution(request, solution)
print(solve_request(request).score_soft)
```

`solve_request` reads `timelimit`, the seed and search options in `settings`
(`models.SolverSettings`), and the rule list in `model_input.rules`. The
lower-level path takes a `es_scheduling.Solver`, a `SolverConfig` and a
`DecomposedCalculator` built from `es_scheduling.constraints`. The C++ side
builds and tests with:

```sh
cmake use-cases/employee-scheduling/cpp -B build/employee-scheduling && cmake --build build/employee-scheduling -j
ctest --test-dir build/employee-scheduling
```

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/es/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`), and this use case registers its own on top:
a smart change move, a window swap between two employees, a hot-employee ruin
and an optional unassign move, all next to the engine construction in
`cpp/src/solver.cpp`. A new move is a `optimaru::MoveOperator<…>` registered
with `engine.addMoveOperator(...)` there; a ruin for LNS is a `RuinOperator`
registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** `seedDayAwareInitialRoster` in
`cpp/src/solver.cpp` places shifts day by day, scarce skills first; the engine's
First Fit then fills whatever is left. Both only touch shifts that are still
unassigned, so pre-assigning part of the solution (`ShiftModel.worker`, or
`with_initial_solution`) keeps that part as the starting point.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/es/constraints/`:
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/es/constraints.hpp` and add it to
   `addDefaultConstraints` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `es_demo` and
   `tests/test_scheduling.py` check they agree;
4. register its name in `addConstraintByName` and `CONSTRAINT_NAMES` in
   `python/bindings.cpp`;
5. add a `Constraint` spec with the snake_case `name` to
   `python/es_scheduling/constraints.py`, and a rule model to
   `python/es_scheduling/models.py` if it should be reachable from JSON.
6. mirror it in the CP-SAT model in
   [`benchmarks/employee-scheduling/cpsat`](../../benchmarks/employee-scheduling/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/employee-scheduling`](../../benchmarks/employee-scheduling) compares this use case with OR-Tools CP-SAT on the same instances.

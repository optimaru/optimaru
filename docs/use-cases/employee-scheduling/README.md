# Employee scheduling

Derived from the Timefold Quickstarts `employee-scheduling` example (Apache-2.0), translated
and modified; see the repository [NOTICE](../../../NOTICE).

Domain model, constraints and API details: [`python.md`](python.md).

<!-- usage-and-extending -->

## Usage

```sh
OPTIMARU_USE_CASES=employee_scheduling uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/employee_scheduling
```

```python
from optimaru import employee_scheduling as es

workers = [es.Employee("Worker 01", {"Skill A"}), es.Employee("Worker 02", {"Skill B"})]
shifts = [
    es.Shift("Shift 1", es.minutes(0, 9), es.minutes(0, 17), required_skill="Skill A"),
    es.Shift("Shift 2", es.minutes(1, 9), es.minutes(1, 12), required_skill="Skill B"),
]
schedule = es.EmployeeSchedule(workers, shifts)
solved = es.solve(schedule, time_limit_seconds=1.0, random_seed=42)   # solves in place and returns the schedule
print(solved.score)                                                   # 0hard/0soft
print([solved.employees[who].name for who in solved.assignment()])    # ['Worker 01', 'Worker 02']
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`, `full_assert`, `on_new_best`, a full `config`
(`es.SolverConfig`, whose fields include `spent_limit_seconds`, `random_seed`,
`parallel_solver_count` and `full_assert`; the keywords override a copy of it) and
`constraints`: a list of constraint specs or a `DecomposedCalculator` (see
[Constraints](#constraints)). With `constraints` the solve is a single search and
`parallel_solver_count` is ignored.

The JSON layer solves a request model:

```python
from optimaru.employee_scheduling.demo import generate_demo_data
from optimaru.employee_scheduling.models import solve_request

request = generate_demo_data()                    # models.ScheduleRequest
request.time_limit_seconds = 2.0
solution = solve_request(request)
print(solution.score_hard, solution.score_soft)
```

`solve_request` reads `time_limit_seconds`, the seed and search options in `settings`
(`models.SolverSettings`), and the constraint list in `model_input.constraints`. The
lower-level path takes a `optimaru.employee_scheduling.Solver`, a `SolverConfig` and a
`DecomposedCalculator` built from `optimaru.employee_scheduling.constraints`. The C++ side
builds and tests with:

```sh
cmake -B build -DOPTIMARU_USE_CASES=employee_scheduling && cmake --build build -j
ctest --test-dir build
```

## Constraints

The constraints (the eight defaults and the opt-in ones, with their default levels and
weights) and the knob table are in [`python.md`](python.md#the-constraints); the table
is not repeated here.

A constraint may be picked several times, with other options or knobs or with the
same ones (every pick is its own instance, and the instances add up), and every constraint
takes the keyword knobs `offset` and `clip`, which shift the per-employee shaping
(`UndesiredDayCost(offset=120)` counts only the minutes beyond two hours;
`clip=False` shapes `cost - offset` as it is, and is refused with an offset on hard,
because hard 0 must mean no violation). `offset=0` is the plain power shaping; see
[`python.md`](python.md) for the knob table.

## Starting from an existing solution

```python
from optimaru.employee_scheduling import with_initial_solution

# Start again from that result: construction only fills what it leaves out.
request = with_initial_solution(request, solution)   # `request`, `solution` from Usage
print(solve_request(request).score_soft)
```

`with_initial_solution` returns a new request and leaves its input untouched.
Workers and shifts are matched by id, a partial solution is fine, and a pinned
shift the solution moves or unassigns raises `ValueError`. Details:
[`python.md`](python.md#starting-from-an-earlier-solution).

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **assignment** shape; the domain solver that configures the
engine is `optimaru/employee_scheduling/cpp/include/es/solver.hpp` + `optimaru/employee_scheduling/cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`), and this use case registers its own on top:
a smart change move, a window swap between two employees, a hot-employee ruin
and an optional unassign move, all next to the engine construction in
`optimaru/employee_scheduling/cpp/src/solver.cpp`. A new move is a `optimaru::MoveOperator<…>` registered
with `engine.add_move_operator(...)` there; a ruin for LNS is a `RuinOperator`
registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** `seedDayAwareInitialRoster` in
`optimaru/employee_scheduling/cpp/src/solver.cpp` places shifts day by day, scarce skills first; the engine's
First Fit then fills whatever is left. Both only touch shifts that are still
unassigned, so pre-assigning part of the solution (`ShiftModel.worker`, or
`with_initial_solution`) keeps that part as the starting point.

**Adding a hard or soft cost.** [Adding a constraint, step by step](extending.md) takes one new
constraint through this use case's files in six steps: changed from Python, written as a
component, tested, made cheaper per move, moved to the default fast path and measured.

## Benchmark

[`benchmarks/employee-scheduling`](../../../benchmarks/employee-scheduling) compares this use case with OR-Tools CP-SAT on the same instances.

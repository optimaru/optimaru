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

`solve` takes the JSON layer's request model too, and returns a
`models.ScheduleSolution`:

```python
from optimaru.employee_scheduling import solve
from optimaru.employee_scheduling.demo import generate_demo_data

request = generate_demo_data()                    # models.ScheduleRequest
solution = solve(request, time_limit_seconds=2.0)
print(solution.score_hard, solution.score_soft)
```

With a request, `solve` reads `timelimit`, the seed and search options in `settings`
(`models.SolverSettings`), and the rule list in `model_input.rules`. The
lower-level path takes a `optimaru.employee_scheduling.Solver`, a `SolverConfig` and a
`DecomposedCalculator` built from `optimaru.employee_scheduling.constraints`. The C++ side
builds and tests with:

```sh
cmake -B build -DOPTIMARU_USE_CASES=employee_scheduling && cmake --build build -j
ctest --test-dir build
```

## Constraints

The rules (the eight defaults and the opt-in ones, with their default levels and
weights) and the knob table are in [`python.md`](python.md#the-rules); the table
is not repeated here.

A rule may be picked several times, with other options or knobs or with the
same ones (every pick is its own instance, and the instances add up), and every rule
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
print(solve(request).score_soft)
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

**Adding a hard or soft cost.** One file per constraint under
`optimaru/employee_scheduling/cpp/include/es/constraints/` (`<subject>_cost.hpp`: `required_skill_cost`,
`employee_unavailability_cost`, `shift_overlap_cost`, `max_one_shift_per_day_cost`,
`min_rest_cost`, `undesired_day_cost`, `desired_day_cost`, `desired_shift_cost`,
`shift_count_imbalance_cost`, `max_consecutive_days_cost`, `min_consecutive_days_cost`,
`max_consecutive_days_off_cost`, `rest_after_consecutive_work_cost`,
`max_weekly_minutes_cost`, `min_weekly_minutes_cost`, `min_weekly_rest_days_cost`,
`assignment_cost`, `unassigned_shifts_cost`, `shift_count_gap_cost`,
`weekly_minutes_gap_cost`, `shift_coverage_cost`, `location_run_length_cost`,
`rest_after_location_run_cost`, `location_run_gap_cost`, `worker_location_count_cost`,
`day_pattern_cost`, `target_days_off_cost`, `worker_day_count_in_range_cost`,
`location_imbalance_cost`, `worker_pairing_cost`; the family bases and `scoped_unit.hpp`
sit next to them):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint`; positive raw cost = penalty, 0 = no violation) declaring
   `static constexpr double kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
   an `explicit <Subject>Cost(double weight = kDefaultWeight, <parameters with neutral
   defaults>)` constructor, the registry name `"<subject>Cost"` (the struct name with a
   lower-case first letter) and, when it is parametrized, a `label` for the instance;
2. include it in `optimaru/employee_scheduling/cpp/include/es/constraints.hpp`, `emplace` it in `catalogConstraints()`
   and, if it belongs to the default set, in `defaultConstraints()`;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` (the `static_assert`s there read the
   components' `kDefaultWeight` / `kDefaultLevel`) — `es_demo` and `es_plugin_test` check
   they agree bit for bit;
4. add its entry to the rule factory in `optimaru/employee_scheduling/cpp/bindings.cpp` (`factory()`: the Python
   name and a builder reading its parameters); `CONSTRAINT_NAMES` and the
   "available:" list follow from that table;
5. add a spec class with the snake_case `name`, `label_en` / `label_ko` /
   `description_en` / `description_ko`, its parameters as dataclass fields and their
   `bounds` to `optimaru/employee_scheduling/constraints.py`, plus its module-level default
   instance and its place in `SPEC_CLASSES`; the JSON rule model is generated from it.
   Add a hand-calculated positive and zero case to `tests/employee_scheduling/test_constraint_costs.py`;
6. mirror it in the CP-SAT model, `external_solver.employee_scheduling` in
   [external-solver](https://github.com/optimaru/external-solver) (a plugin subclassing the
   spec; defaults come from `constraint_info()`), so the benchmark in
   [`benchmarks/employee-scheduling/cpsat`](../../../benchmarks/employee-scheduling/cpsat)
   stays like for like.

## Benchmark

[`benchmarks/employee-scheduling`](../../../benchmarks/employee-scheduling) compares this use case with OR-Tools CP-SAT on the same instances.

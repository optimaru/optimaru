# Employee Scheduling

Derived from the Timefold Quickstarts `employee-scheduling` example (Apache-2.0), translated
and modified; see the repository [NOTICE](../../NOTICE).

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

**0.3.0:** a rule may be picked several times, with other options or knobs or with the
same ones (every pick is its own instance, and the instances add up), and every rule
takes two more keyword knobs, `offset` and `clip`, which shift the per-employee shaping
(`UndesiredDayCost(offset=120)` counts only the minutes beyond two hours;
`clip=False` shapes `cost - offset` as it is, and is refused with an offset on hard,
because hard 0 must mean no violation). `offset=0` is the 0.2.0 shaping; see
[`python/`](python/README.md) for the knob table.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-use-case).
The solution has the **assignment** shape; the domain solver that configures the
engine is `cpp/include/es/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`), and this use case registers its own on top:
a smart change move, a window swap between two employees, a hot-employee ruin
and an optional unassign move, all next to the engine construction in
`cpp/src/solver.cpp`. A new move is a `optimaru::MoveOperator<…>` registered
with `engine.add_move_operator(...)` there; a ruin for LNS is a `RuinOperator`
registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** `seedDayAwareInitialRoster` in
`cpp/src/solver.cpp` places shifts day by day, scarce skills first; the engine's
First Fit then fills whatever is left. Both only touch shifts that are still
unassigned, so pre-assigning part of the solution (`ShiftModel.worker`, or
`with_initial_solution`) keeps that part as the starting point.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/es/constraints/` (`<subject>_cost.hpp`: `required_skill_cost`,
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
2. include it in `cpp/include/es/constraints.hpp`, `emplace` it in `catalogConstraints()`
   and, if it belongs to the default set, in `defaultConstraints()`;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` (the `static_assert`s there read the
   components' `kDefaultWeight` / `kDefaultLevel`) — `es_demo` and `es_plugin_test` check
   they agree bit for bit;
4. add its entry to the rule factory in `python/bindings.cpp` (`factory()`: the Python
   name and a builder reading its parameters); `CONSTRAINT_NAMES` and the
   "available:" list follow from that table;
5. add a spec class with the snake_case `name`, `label_en` / `label_ko` /
   `description_en` / `description_ko`, its parameters as dataclass fields and their
   `bounds` to `python/es_scheduling/constraints.py`, plus its module-level default
   instance and its place in `SPEC_CLASSES`; the JSON rule model is generated from it.
   Add a hand-calculated positive and zero case to `tests/test_constraint_costs.py`;
6. mirror it in the CP-SAT model in
   [`benchmarks/employee-scheduling/cpsat`](../../benchmarks/employee-scheduling/cpsat) (a
   plugin subclassing the spec; defaults come from `constraint_info()`) so the benchmark
   stays like for like.

## Benchmark

[`benchmarks/employee-scheduling`](../../benchmarks/employee-scheduling) compares this use case with OR-Tools CP-SAT on the same instances.

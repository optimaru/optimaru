# Semiconductor fab scheduling

Independent semiconductor-fab scheduling use case with re-entrant lot routes,
alternative tools, sequence-dependent recipe setups, simultaneous batches,
inter-tool transport, queue-time limits, preventive maintenance, priorities
and due dates. It also models reusable operators/reticles/fixtures, their
capacities and calendars, frozen assignments for replanning, minimum hold times
and maximum lot cycle times. The Python package contains the canonical model, constraints and
metaheuristic solver; `benchmarks/semiconductor-scheduling/cpsat` contains the exact reference.

The model combines patterns documented by the official
[Hexaly Flexible Job Shop example](https://www.hexaly.com/docs/last/exampletour/flexiblejobshop.html),
[Timefold Task Scheduling constraints](https://docs.timefold.ai/job-scheduling/latest/user-guide/constraints),
the [Timefold employee-resource model](https://docs.timefold.ai/job-scheduling/latest/machine-resource-constraints/employee-resources),
the [IBM CP scheduling example library](https://www.ibm.com/docs/en/icos/22.2.0?topic=programming-where-find-cp-examples),
and [Gurobi's modeling examples](https://github.com/Gurobi/modeling-examples).
The implementation is original and does not depend on a commercial runtime.

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/semiconductor-scheduling/python
uv sync --all-groups --all-extras
uv run pytest -q
```

```python
from semiconductor_scheduling.models import SemiconductorScheduleModel

problem = SemiconductorScheduleModel.model_validate_json(open("fab.json").read())
from semiconductor_scheduling import solve

print(solve(problem, time_limit_seconds=2.0, random_seed=42))   # (hard, soft); fills problem.assignments
print([(a.lot_id, a.operation_id, a.machine_id, a.start) for a in problem.assignments])
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`,
a full `config` (`SolverConfig`: temperature, cooling rate, stagnation
limit, …) and `constraints`.

## Extending

This is a pure Python reference implementation (simulated annealing in
`python/semiconductor_scheduling/common.py`); it does not use the C++ engine.

- **Adding a move** — extend `neighbor(state, rng)` inside `solve()` in
  `python/semiconductor_scheduling/__init__.py`; it returns one random neighbour of the
  current state, so a new kind of change is another branch chosen with `rng`.
- **Changing the construction heuristic** — the initial state built at the
  top of `solve()` in the same file.
- **Adding a hard or soft cost** — add a `Constraint` subclass (`name`,
  `label`, `default_hard`) to `python/semiconductor_scheduling/constraints.py` and add it to the
  default list there (`DEFAULTS`), then return its raw cost under the same `name`
  from `constraint_costs()` in `__init__.py`. Mirror it in
  [`benchmarks/semiconductor-scheduling/cpsat`](../../benchmarks/semiconductor-scheduling/cpsat).

## Benchmark

[`benchmarks/semiconductor-scheduling`](../../benchmarks/semiconductor-scheduling) compares this use case with OR-Tools CP-SAT on the same instances, rescored by the same evaluator.

Latest run (30 s per solver, 2 instances): best score by the engine on 0, CP-SAT on 2 (ties count for each). Full table: [benchmarks/semiconductor-scheduling/README.md](../../benchmarks/semiconductor-scheduling/README.md#results).

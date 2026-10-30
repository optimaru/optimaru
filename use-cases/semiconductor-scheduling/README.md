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

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose
search starts from `solution`, a solved model or the same thing as a JSON dict.
Assignments are matched by lot and operation id. Any result of `solve` is
accepted, feasible or not. Unknown ids, duplicates, ineligible machines and wrong
durations raise `ValueError`.

```python
from semiconductor_scheduling import with_initial_solution

warm = with_initial_solution(problem, problem.model_dump(mode="json"))
print(solve(warm, time_limit_seconds=2.0, random_seed=42))   # (hard, soft)
print(warm.makespan, warm.total_weighted_tardiness)
```

The search works on machine choices and priority ranks that a decoder turns into
a schedule, and the decoder left-shifts operations and re-derives batches, so
decoding a seed can give a different schedule, sometimes a worse one. A complete
seed is therefore scored as supplied: `solve` returns it unchanged unless the
search finds a strictly better schedule, so the score is never worse than the
seed's. The search itself is not guaranteed to stay near the seed or to improve
it. A partial seed cannot be scored, so it is decoded: it keeps its machines and
relative order, but its start times are recomputed.

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

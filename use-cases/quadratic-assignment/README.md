# Quadratic assignment

Independent quadratic assignment use case. `python/quadratic_assignment`
provides the canonical model, constraints and metaheuristic solver;
`benchmarks/quadratic-assignment/cpsat` provides the exact reference implementation.

Constraints cover quadratic flow-distance cost, direct assignment cost, fixed
and forbidden assignments, and facility-demand/location-capacity compatibility.
The model is aligned with the quadratic-assignment examples listed in the
[IBM CP Optimizer model library](https://www.ibm.com/docs/en/icos/22.2.0?topic=programming-where-find-cp-examples)
and Gurobi's public quadratic-assignment modeling examples.

## QAPLIB benchmark used by Hexaly

`benchmarks/quadratic-assignment/bench/run_bench.py` downloads the same QAP template archive published by
Hexaly and the corresponding QAPLIB solution archive, verifies both SHA-256
checksums, then re-scores every result with this package's constraint
calculator. Downloaded files stay in `benchmarks/quadratic-assignment/bench/.cache/` and are not committed.

```sh
cd benchmarks/quadratic-assignment
uv sync --project cpsat --all-groups
uv run --project cpsat python -m bench.run_bench --seconds 60
# Select more instances; CP-SAT is optional because its reference QAP model is O(n^4).
uv run --project cpsat python -m bench.run_bench \
  --instance nug30 --instance lipa40a --exact-max-size 32 --seconds 60
```

The report shows the raw objective and gap to the QAPLIB best-known solution.
Hexaly's website publishes a 1.1% aggregate gap at 60 seconds, not its
per-instance assignments, so the script labels that number as an external
aggregate rather than pretending it is a directly reproduced solver run.
The checked-in local snapshot is in [`bench/RESULTS.md`](../../benchmarks/quadratic-assignment/bench/RESULTS.md).

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/quadratic-assignment/python
uv sync --all-groups --all-extras
uv run pytest -q
```

```python
from quadratic_assignment import solve
from quadratic_assignment.models import QuadraticAssignmentProblemModel

problem = QuadraticAssignmentProblemModel(
    facilities=["F1", "F2"], locations=["L1", "L2"],
    flows=[[0, 3], [2, 0]], distances=[[0, 4], [5, 0]],
)
solve(problem, time_limit_seconds=1.0, random_seed=42)
print(problem.assignment, problem.total_cost)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`,
a full `config` (`SolverConfig`: temperature, cooling rate, stagnation
limit, …) and `constraints`.

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose
search starts from `solution`, a solved model or the same thing as a JSON dict.
Its `assignment` must be a full bijection from locations to facilities: partial
assignments, unknown ids and repeated facilities raise `ValueError`.

```python
from quadratic_assignment import with_initial_solution

warm = with_initial_solution(problem, problem.model_dump(mode="json"))
solve(warm, time_limit_seconds=1.0, random_seed=42)
print(warm.assignment, warm.total_cost)
```

## Extending

This is a pure Python reference implementation (simulated annealing in
`python/quadratic_assignment/common.py`); it does not use the C++ engine.

- **Adding a move** — extend `neighbor(state, rng)` inside `solve()` in
  `python/quadratic_assignment/__init__.py`; it returns one random neighbour of the
  current state, so a new kind of change is another branch chosen with `rng`.
- **Changing the construction heuristic** — the initial state built at the
  top of `solve()` in the same file.
- **Adding a hard or soft cost** — add a `Constraint` subclass (`name`,
  `label`, `default_hard`) to `python/quadratic_assignment/constraints.py` and add it to the
  default list there (`DEFAULTS`), then return its raw cost under the same `name`
  from `constraint_costs()` in `__init__.py`. Mirror it in
  [`benchmarks/quadratic-assignment/cpsat`](../../benchmarks/quadratic-assignment/cpsat).

## Benchmark

[`benchmarks/quadratic-assignment`](../../benchmarks/quadratic-assignment) compares this use case with
OR-Tools CP-SAT on public instances.

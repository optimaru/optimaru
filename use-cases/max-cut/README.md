# Max-Cut

Independent weighted Max-Cut use case. `python/max_cut` provides the canonical
model, constraints and metaheuristic solver; `benchmarks/max-cut/cpsat` provides the
exact reference implementation.

Constraints cover weighted cut maximization, fixed vertex sides, same-side and
different-side pairs, partition cardinality bounds and optional balance. The
base formulation follows the official
[Hexaly weighted Max-Cut template](https://www.hexaly.com/templates/max-cut-problem).

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/max-cut/python
uv sync --all-groups --all-extras
uv run pytest -q
```

```python
from max_cut import solve
from max_cut.models import EdgeModel, MaxCutProblemModel

problem = MaxCutProblemModel(
    vertices=["A", "B", "C"],
    edges=[EdgeModel(source="A", target="B", weight=2), EdgeModel(source="B", target="C", weight=3),
           EdgeModel(source="A", target="C", weight=4)],
)
solve(problem, time_limit_seconds=1.0, random_seed=42)
print(problem.partition, problem.cut_weight)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`,
a full `config` (`SolverConfig`: temperature, cooling rate, stagnation
limit, …) and `constraints`.

## Extending

This is a pure Python reference implementation (simulated annealing in
`python/max_cut/common.py`); it does not use the C++ engine.

- **Adding a move** — extend `neighbor(state, rng)` inside `solve()` in
  `python/max_cut/__init__.py`; it returns one random neighbour of the
  current state, so a new kind of change is another branch chosen with `rng`.
- **Changing the construction heuristic** — the initial state built at the
  top of `solve()` in the same file.
- **Adding a hard or soft cost** — add a `Constraint` subclass (`name`,
  `label`, `default_hard`) to `python/max_cut/constraints.py` and add it to the
  default list there (`DEFAULTS`), then return its raw cost under the same `name`
  from `constraint_costs()` in `__init__.py`. Mirror it in
  [`benchmarks/max-cut/cpsat`](../../benchmarks/max-cut/cpsat).

## Benchmark

[`benchmarks/max-cut`](../../benchmarks/max-cut) compares this use case with OR-Tools CP-SAT on the same instances, rescored by the same evaluator.

Latest run (30 s per solver, 3 instances): best score by the engine on 0, CP-SAT on 3 (ties count for each). Full table: [benchmarks/max-cut/README.md](../../benchmarks/max-cut/README.md#results).

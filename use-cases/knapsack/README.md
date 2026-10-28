# Knapsack

Independent 0/1 knapsack use case. `python/knapsack` provides the canonical
model, constraints and metaheuristic solver; `benchmarks/knapsack/cpsat` provides the
exact reference implementation.

Constraints cover primary and multidimensional resource capacities, mandatory
and forbidden items, lower/upper selection-group bounds, item dependencies and
total-value maximization. The basic formulation follows the official
[Hexaly knapsack template](https://www.hexaly.com/templates/knapsack-problem);
the extra contracts expose common multiple-choice and multidimensional variants
without changing the default instance format.

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/knapsack/python
uv sync --all-groups --all-extras
uv run pytest -q
```

```python
from knapsack import solve
from knapsack.models import ItemModel, KnapsackProblemModel

problem = KnapsackProblemModel(
    capacity=10,
    items=[ItemModel(id="A", weight=6, value=9), ItemModel(id="B", weight=4, value=7),
           ItemModel(id="C", weight=5, value=8)],
)
print(solve(problem, time_limit_seconds=1.0, random_seed=42))   # (hard, soft); solves in place
print([item.id for item in problem.items if item.selected], problem.total_value)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`,
a full `config` (`SolverConfig`: temperature, cooling rate, stagnation
limit, …) and `constraints`.

## Extending

This is a pure Python reference implementation (simulated annealing in
`python/knapsack/common.py`); it does not use the C++ engine.

- **Adding a move** — extend `neighbor(state, rng)` inside `solve()` in
  `python/knapsack/__init__.py`; it returns one random neighbour of the
  current state, so a new kind of change is another branch chosen with `rng`.
- **Changing the construction heuristic** — the initial state built at the
  top of `solve()` in the same file.
- **Adding a hard or soft cost** — add a `Constraint` subclass (`name`,
  `label`, `default_hard`) to `python/knapsack/constraints.py` and add it to the
  default list there (`DEFAULTS`), then return its raw cost under the same `name`
  from `constraint_costs()` in `__init__.py`. Mirror it in
  [`benchmarks/knapsack/cpsat`](../../benchmarks/knapsack/cpsat).

## Benchmark

[`benchmarks/knapsack`](../../benchmarks/knapsack) compares this use case with
OR-Tools CP-SAT on public instances.

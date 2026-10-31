# Bin packing

Uniform-capacity one-dimensional bin packing, based on the compact model used
in the [Hexaly BPP template](https://www.hexaly.com/templates/bin-packing-problem-bpp).

Every item is assigned to exactly one bin by the solution representation. Four
score plugins remain selectable:

- `bin_capacity` (hard): total overload across bins.
- `color_compatibility` (hard): at most one item of each color per bin.
- `item_conflicts` (hard): forbidden item pairs cannot share a bin.
- `minimize_bins` (soft): number of non-empty bins.

The canonical `bin_packing` package contains the model, shared constraint
classes, evaluator, and a local-search solver. `benchmarks/bin-packing/cpsat` implements
the same plugins with OR-Tools CP-SAT.

## Public instances shipped by Hexaly

`benchmarks/bin-packing/bench/run_bench.py` downloads and checksum-verifies Hexaly's BPP template
archive, then compares this engine with CP-SAT on the same `t/u` instances.
Both answers are independently checked for overload and reported against the
capacity lower bound `ceil(total weight / capacity)`.

```sh
cd benchmarks/bin-packing
uv sync --project cpsat --all-groups
uv run --project cpsat python -m bench.run_bench --seconds 60
uv run --project cpsat python -m bench.run_bench \
  --all --seconds 60 --exact-max-size 0 --json
```

These template files are not the 240-instance BPPLIB GI collection used in
Hexaly's 2024 performance article, so the runner does not mix their metrics or
claim a direct comparison with the published 0.1% aggregate gap.

## Usage

```sh
cd use-cases/bin-packing/python
uv sync --all-groups --all-extras
uv run pytest -q
```

```python
from bin_packing import BinPackingProblem, Item, solve

problem = BinPackingProblem(10, [Item("A", 8), Item("B", 7), Item("C", 3), Item("D", 2)])
print(solve(problem, time_limit_seconds=1.0, random_seed=42))   # Score(hard, soft)
print(problem.bins_used(), problem.bin_loads())
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`,
a full `config` (`SolverConfig`: temperature, cooling rate, stagnation
limit, …) and `constraints`.

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of a
`BinPackingProblemModel` whose search starts from `solution`, a solved model or
the same thing as a JSON dict. Items are matched by id; items left out or at
bin -1 are first-fitted around the seeded bins before the search starts, and a
complete seed skips construction. Stagnation restarts return to the seed instead
of a new first fit. Unknown ids, duplicates and bins outside the item count raise
`ValueError`.

```python
from bin_packing import with_initial_solution
from bin_packing.models import BinPackingProblemModel

solved = BinPackingProblemModel.from_core(problem)
warm = with_initial_solution(solved, solved.model_dump(mode="json")).to_core()
print(solve(warm, time_limit_seconds=1.0, random_seed=42), warm.bin_loads())
```

## Extending

This is a pure Python reference implementation; it does not use the C++
engine. The solver is `Solver.solve_with_constraints` in
`python/bin_packing/_core.py`.

- **Adding a move** — the annealing loop there draws one change per step
  (`_try_empty_bin`, which empties the lightest bin, a random reassignment,
  or a reassignment plus swap); add a new kind of change as another branch.
- **Changing the construction heuristic** — the initial assignment is
  `_first_fit` over items sorted by decreasing weight (further starts and
  stagnation restarts reshuffle the order).
- **Adding a hard or soft cost** — add a `Constraint` subclass with `name`
  to `python/bin_packing/constraints.py` and to the default list in
  `get_constraints()`, then return `{"hard", "weight", "costs"}` under the
  same `name` from `constraint_costs()` in `_core.py`. Mirror it in
  [`benchmarks/bin-packing/cpsat`](../../benchmarks/bin-packing/cpsat).

## Benchmark

[`benchmarks/bin-packing`](../../benchmarks/bin-packing) compares this use case with
OR-Tools CP-SAT on public instances.

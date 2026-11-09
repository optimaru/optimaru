# Knapsack benchmark

The [`knapsack`](../../use-cases/knapsack) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `knapsack_cpsat`). The CP-SAT model implements the
same constraints under the same names and units — one external-solver
`ConstraintPlugin` per constraint ([`external_solver.knapsack.constraints`](https://github.com/optimaru/external-solver/tree/main/src/external_solver/knapsack/constraints.py)), each a
subclass of the domain spec with its defaults read from
`knapsack.constraint_info()` — so both answers are rescored by the use case's
own evaluator. `cpsat/tests/test_constraint_parity.py` pins every selection of
small instances (and 200 random ones of a 10-item instance) and checks that each
constraint's CP-SAT term equals the engine's `constraint_costs()` total (0 exactly
when the engine reports no violation), and that the CP-SAT optimum equals the
brute-force optimum of the engine's score, per constraint and for the default
set. Exponents other than 1 and fractional weights are not supported on CP-SAT
(they raise `ValueError`); `level="soft"` moves a hard term to soft in both solvers,
`level="medium"` is refused by both (`this use case has no medium level`), and
`selected_value_cost` with `level="hard"` is refused as in the engine.

A constraint may be picked several times (as specs or as
`{"name", "weight", "level", "offset", "clip"}` mappings); each pick builds its own term with
its own knobs, its level included. Every knapsack constraint is global and adds its single
total as one term, so an integral `offset` shapes that total exactly as the engine does in
both `clip` modes; a fractional offset is refused (`offset must be an integer for CP-SAT`).
`clip=False` with an offset on a hard pick is refused with the engine's message when the
objective is built.

```sh
cd benchmarks/knapsack/cpsat
uv sync --all-groups --all-extras
uv run pytest -q                       # CP-SAT model + engine parity tests
uv run knapsack-compare --seconds 10 --workers 8   # engine vs CP-SAT table (--markdown, --json)
```

The engine runs single-threaded; CP-SAT gets `--workers` search workers and
its time includes model construction.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| n50 | 50 items | **0hard/1569soft** | **0hard/1569soft** |
| n200 | 200 items | **0hard/6899soft** | **0hard/6899soft** |
| n1000 | 1000 items | 0hard/34333soft | **0hard/34336soft** |

Measured 2026-11-04 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — treat small differences as noise.
<!-- results:end -->

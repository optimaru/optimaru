# Knapsack benchmark

The [`knapsack`](../../use-cases/knapsack) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `knapsack_cpsat`). The CP-SAT model implements the
same constraints under the same names and units — one `cpsat_plugins`
`ConstraintPlugin` per constraint (`knapsack_cpsat/constraints.py`), each a
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

**0.3.0:** a constraint may be picked several times (as specs or as
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

Scores come from the use case's own evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| n50 | 50 items | **0hard/1569soft** | **0hard/1569soft** |
| n200 | 200 items | **0hard/6899soft** | **0hard/6899soft** |
| n1000 | 1000 items | 0hard/34329soft | **0hard/34336soft** |

Measured 2026-10-31 with the 0.2.0 C++ engine on a shared 4-core Linux container,
10 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses
2 workers and its time includes model construction. Single runs — treat small
differences as noise. (The 0.1.5 pure-Python engine, 30 s on a 16-core Mac with
CP-SAT on 8 workers, reached 1557 / 6885 / 34329 against CP-SAT's 1569 / 6899 /
34336.)
<!-- results:end -->

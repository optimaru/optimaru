# Knapsack benchmark

The [`knapsack`](../../use-cases/knapsack) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `knapsack_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in `cpsat/tests`
keep the two cost definitions identical.

```sh
cd benchmarks/knapsack/cpsat
uv sync --all-groups --all-extras      # builds the engine extension too
uv run pytest -q                       # CP-SAT model + parity tests
uv run knapsack-compare --seconds 10 --workers 8   # engine vs CP-SAT table (--markdown, --json)
```

The engine runs single-threaded; CP-SAT gets `--workers` search workers and
its time includes model construction.

<!-- results:start -->
## Results

Scores come from the use case's own evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| n50 | 50 items | 0hard/1557soft | **0hard/1569soft** |
| n200 | 200 items | 0hard/6885soft | **0hard/6899soft** |
| n1000 | 1000 items | 0hard/34329soft | **0hard/34336soft** |

Measured 2026-10-29 on one Apple Silicon Mac (16 cores), 30 s per solver and instance, seed 1. The engine run single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — treat small differences as noise.
<!-- results:end -->

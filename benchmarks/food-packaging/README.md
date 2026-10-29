# Food packaging benchmark

The [`food-packaging`](../../use-cases/food-packaging) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `food_packaging_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in `cpsat/tests`
keep the two cost definitions identical.

```sh
cd benchmarks/food-packaging/cpsat
uv sync --all-groups --all-extras      # builds the engine extension too
uv run pytest -q                       # CP-SAT model + parity tests
uv run food-packaging-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| demo | Java quickstart demo plant | **0hard/0medium/-95221284soft** | — (no solution in time) |

Measured 2026-10-29 on one Apple Silicon Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — treat small differences as noise.
<!-- results:end -->

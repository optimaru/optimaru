# Semiconductor scheduling benchmark

The [`semiconductor-scheduling`](../../use-cases/semiconductor-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `semiconductor_scheduling_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in `cpsat/tests`
keep the two cost definitions identical.

```sh
cd benchmarks/semiconductor-scheduling/cpsat
uv sync --all-groups --all-extras      # builds the engine extension too
uv run pytest -q                       # CP-SAT model + parity tests
uv run semiconductor-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT table (--markdown, --json)
```

The engine runs single-threaded; CP-SAT gets `--workers` search workers and
its time includes model construction.

<!-- results:start -->
## Results

Scores come from the use case's own evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| lots6 | 6 lots / 6 machines | 0hard/-167soft | **0hard/-163soft** |
| lots12 | 12 lots / 6 machines | 0hard/-437soft | **0hard/-415soft** |

Measured 2026-10-29 on one Apple Silicon Mac (16 cores), 30 s per solver and instance, seed 1. The engine run single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — treat small differences as noise.
<!-- results:end -->

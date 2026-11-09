# Semiconductor scheduling benchmark

The [`semiconductor-scheduling`](../../use-cases/semiconductor-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `semiconductor_scheduling_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, as external-solver plugins of the domain
specs, so both answers are rescored by the use case's own C++ evaluator; external-solver's
parity tests (`tests/semiconductor_scheduling/test_parity.py`) keep the two cost definitions
identical (formulation notes:
[`cpsat/README.md`](cpsat/README.md)).

```sh
cd benchmarks/semiconductor-scheduling/cpsat
uv sync --all-groups --all-extras      # builds the engine extension too
uv run pytest -q                       # benchmark harness tests
uv run semiconductor-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT table (--markdown, --json)
```

The engine runs single-threaded; CP-SAT gets `--workers` search workers and
its time includes model construction.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| lots6 | 6 lots / 6 machines | **0hard/-163soft** | **0hard/-163soft** |
| lots12 | 12 lots / 6 machines | 0hard/-415soft | **0hard/-411soft** |

Measured 2026-11-04 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — treat small differences as noise.
<!-- results:end -->

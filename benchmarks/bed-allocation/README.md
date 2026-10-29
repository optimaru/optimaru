# Bed allocation benchmark

The [`bed-allocation`](../../use-cases/bed-allocation) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `bed_allocation_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in `cpsat/tests`
keep the two cost definitions identical.

```sh
cd benchmarks/bed-allocation/cpsat
uv sync --all-groups --all-extras      # builds the engine extension too
uv run pytest -q                       # CP-SAT model + parity tests
uv run bed-allocation-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

`dump_instances.py` writes the comparison instances for the engine-only
C++ bench (`use-cases/bed-allocation/cpp/bench/ba_quality_bench.cpp`).

The engine runs single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| demo | 77 stays / 13 beds / 10 rooms | 0hard/0medium/-6954soft | **0hard/0medium/-6640soft** |
| medium | 183 stays / 30 beds / 24 rooms | **0hard/0medium/-13686soft** | 0hard/0medium/-21316soft |
| large | 551 stays / 61 beds / 48 rooms | **0hard/0medium/-34556soft** | — (no solution in time) |

Measured 2026-10-29 on one Apple Silicon Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — treat small differences as noise.
<!-- results:end -->

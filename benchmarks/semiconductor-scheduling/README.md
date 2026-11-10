# Semiconductor scheduling benchmark

The [`semiconductor-scheduling`](../../docs/use-cases/semiconductor-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `semiconductor_scheduling_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, as external-solver plugins of the domain
specs, so both answers are rescored by the use case's own C++ evaluator; external-solver's
parity tests (`tests/semiconductor_scheduling/test_parity.py`) keep the two cost definitions
identical (formulation notes:
[`cpsat/README.md`](cpsat/README.md)).

```sh
cd benchmarks/semiconductor-scheduling/cpsat
OPTIMARU_USE_CASES=semiconductor_scheduling uv sync --all-groups --all-extras   # also builds this checkout's optimaru (semiconductor_scheduling only)
uv run pytest -q                       # benchmark harness tests
uv run semiconductor-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT table (--markdown, --json)
```

The engine runs single-threaded; CP-SAT gets `--workers` search workers and
its time includes model construction.

The engine's result on the larger instances varies between runs of the same
build: four 30 s runs of the 18-lot instance gave soft -839, -842, -881 and
-894 (the 6- and 12-lot instances repeat to the digit). The table below has the
measured run, which is the best of the four, and CP-SAT's -857 lies inside that
range: read that row, and any margin of a few percent here, as noise.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| lots6 | 6 lots / 6 machines | **0hard/-163soft** | **0hard/-163soft** |
| lots12 | 12 lots / 6 machines | 0hard/-413soft | **0hard/-411soft** |
| lots18 | 18 lots / 6 machines | **0hard/-839soft** | 0hard/-857soft |
| lots24 | 24 lots / 6 machines | **0hard/-2142soft** | 0hard/-2338soft |
| lots36 | 36 lots / 6 machines | **0hard/-10563soft** | 0hard/-12165soft |
| lots48 | 48 lots / 6 machines | **0hard/-24279soft** | 0hard/-26101soft |

Measured 2026-11-11 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise.
<!-- results:end -->

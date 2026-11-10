# Bed allocation benchmark

Transcribes the Timefold Quickstarts `bed-allocation` example's demo data generator (Apache-2.0),
with modifications; see the repository [NOTICE](../../NOTICE).

The [`bed-allocation`](../../docs/use-cases/bed-allocation) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `bed_allocation_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in external-solver
(`tests/bed_allocation/`) keep the two cost definitions identical.

```sh
cd benchmarks/bed-allocation/cpsat
OPTIMARU_USE_CASES=bed_allocation uv sync --all-groups --all-extras   # also builds this checkout's optimaru (bed_allocation only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run bed-allocation-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

`dump_instances.py` writes the comparison instances for the engine-only
C++ bench (`optimaru/bed_allocation/cpp/bench/ba_quality_bench.cpp`).

The engine runs single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| demo | 77 stays / 13 beds / 10 rooms | 0hard/0medium/-6954soft | **0hard/0medium/-6640soft** |
| medium | 183 stays / 30 beds / 24 rooms | **0hard/0medium/-13686soft** | 0hard/0medium/-21696soft |
| large | 551 stays / 61 beds / 48 rooms | **0hard/0medium/-34556soft** | — (no solution in time) |

Measured 2026-11-04 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — treat small differences as noise.
<!-- results:end -->

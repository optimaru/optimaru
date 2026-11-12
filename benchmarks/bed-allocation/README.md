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
OPTIMARU_USE_CASES=bed_allocation uv sync   # also builds this checkout's optimaru (bed_allocation only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run bed-allocation-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

`dump_instances.py` writes the comparison instances for the engine-only
C++ bench ([`cpp/ba_quality_bench.cpp`](cpp/ba_quality_bench.cpp)).

The engine runs single-threaded. CP-SAT gets `--workers` search workers;
its budget starts once its model is built, and the time the runner reports
for it includes building the model.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| small | 20 stays / 6 beds / 5 rooms | **0hard/0medium/-3580soft** | **0hard/0medium/-3580soft** |
| small-2 | 19 stays / 6 beds / 5 rooms | **0hard/0medium/-3270soft** | **0hard/0medium/-3270soft** |
| small-3 | 19 stays / 6 beds / 5 rooms | **0hard/0medium/-3310soft** | **0hard/0medium/-3310soft** |
| small-4 | 20 stays / 6 beds / 5 rooms | **0hard/-3medium/-4600soft** | **0hard/-3medium/-4600soft** |
| demo | 77 stays / 13 beds / 10 rooms | **0hard/0medium/-6640soft** | **0hard/0medium/-6640soft** |
| demo-2 | 76 stays / 13 beds / 10 rooms | **0hard/0medium/-7404soft** | **0hard/0medium/-7404soft** |
| demo-3 | 75 stays / 13 beds / 10 rooms | 0hard/0medium/-9054soft | **0hard/0medium/-9046soft** |
| demo-4 | 79 stays / 13 beds / 10 rooms | 0hard/0medium/-10434soft | **0hard/0medium/-10282soft** |
| medium | 183 stays / 30 beds / 24 rooms | **0hard/0medium/-13192soft** | 0hard/0medium/-20144soft |
| medium-2 | 180 stays / 30 beds / 24 rooms | **0hard/0medium/-15676soft** | 0hard/0medium/-24506soft |
| medium-3 | 183 stays / 30 beds / 24 rooms | **0hard/0medium/-11178soft** | 0hard/0medium/-18522soft |
| medium-4 | 182 stays / 30 beds / 24 rooms | **0hard/0medium/-12394soft** | 0hard/0medium/-19800soft |
| medium-5 | 187 stays / 30 beds / 24 rooms | **0hard/0medium/-13882soft** | 0hard/0medium/-22574soft |
| large | 551 stays / 61 beds / 48 rooms | **0hard/0medium/-34932soft** | — (no solution in time) |
| large-2 | 546 stays / 61 beds / 48 rooms | **0hard/0medium/-29166soft** | — (no solution in time) |
| large-3 | 550 stays / 61 beds / 48 rooms | **0hard/0medium/-27708soft** | — (no solution in time) |
| large-4 | 549 stays / 61 beds / 48 rooms | **0hard/0medium/-24552soft** | — (no solution in time) |
| xlarge | 1233 stays / 104 beds / 80 rooms | **0hard/0medium/-35814soft** | — (no solution in time) |
| xlarge-2 | 1218 stays / 104 beds / 80 rooms | **0hard/0medium/-55276soft** | — (no solution in time) |
| xlarge-3 | 1216 stays / 104 beds / 80 rooms | **0hard/0medium/-36988soft** | — (no solution in time) |

Measured 2026-11-12 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

# Maintenance scheduling benchmark

The [`maintenance-scheduling`](../../docs/use-cases/maintenance-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `maintenance_scheduling_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in external-solver
(`tests/maintenance_scheduling/`) keep the two cost definitions identical.

```sh
cd benchmarks/maintenance-scheduling/cpsat
OPTIMARU_USE_CASES=maintenance_scheduling uv sync   # also builds this checkout's optimaru (maintenance_scheduling only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run maintenance-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded. CP-SAT gets `--workers` search workers;
its budget starts once its model is built, and the time the runner reports
for it includes building the model.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| small | 14 jobs / 3 crews / 40 dates | 0hard/-3145soft | **0hard/-3133soft** |
| small-2 | 14 jobs / 3 crews / 40 dates | 0hard/-14103soft | **0hard/-14098soft** |
| small-3 | 14 jobs / 3 crews / 40 dates | **0hard/-8149soft** | **0hard/-8149soft** |
| small-4 | 14 jobs / 3 crews / 40 dates | **0hard/-24141soft** | **0hard/-24141soft** |
| small-5 | 14 jobs / 3 crews / 40 dates | **0hard/-9083soft** | **0hard/-9083soft** |
| small-6 | 14 jobs / 3 crews / 40 dates | 0hard/-105soft | **0hard/-102soft** |
| medium | 28 jobs / 4 crews / 60 dates | **0hard/-11236soft** | — (no solution in time) |
| medium-2 | 28 jobs / 4 crews / 60 dates | **0hard/-10288soft** | — (no solution in time) |
| medium-3 | 28 jobs / 4 crews / 60 dates | **0hard/-9094soft** | — (no solution in time) |
| medium-4 | 28 jobs / 4 crews / 60 dates | **0hard/-216soft** | — (no solution in time) |
| medium-5 | 28 jobs / 4 crews / 60 dates | **0hard/-6135soft** | — (no solution in time) |
| medium-6 | 28 jobs / 4 crews / 60 dates | **0hard/-16253soft** | — (no solution in time) |
| large | 48 jobs / 5 crews / 80 dates | **0hard/-261soft** | — (no solution in time) |
| large-2 | 48 jobs / 5 crews / 80 dates | **0hard/-396soft** | — (no solution in time) |
| large-3 | 48 jobs / 5 crews / 80 dates | **0hard/-298soft** | — (no solution in time) |
| large-4 | 48 jobs / 5 crews / 80 dates | **0hard/-1291soft** | — (no solution in time) |
| large-5 | 48 jobs / 5 crews / 80 dates | **0hard/-11188soft** | — (no solution in time) |
| xlarge | 96 jobs / 8 crews / 120 dates | **0hard/-8374soft** | — (no solution in time) |
| xlarge-2 | 96 jobs / 8 crews / 120 dates | **0hard/-8517soft** | — (no solution in time) |
| xlarge-3 | 96 jobs / 8 crews / 120 dates | **0hard/-30551soft** | — (no solution in time) |

Measured 2026-11-12 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

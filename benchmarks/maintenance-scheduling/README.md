# Maintenance scheduling benchmark

The [`maintenance-scheduling`](../../docs/use-cases/maintenance-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `maintenance_scheduling_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in external-solver
(`tests/maintenance_scheduling/`) keep the two cost definitions identical.

```sh
cd benchmarks/maintenance-scheduling/cpsat
OPTIMARU_USE_CASES=maintenance_scheduling uv sync --all-groups --all-extras   # also builds this checkout's optimaru (maintenance_scheduling only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run maintenance-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| small | 14 jobs / 3 crews / 40 dates | 0hard/-5139soft | **0hard/-3133soft** |
| large | 48 jobs / 5 crews / 80 dates | **0hard/-259soft** | — (no solution in time) |

Measured 2026-11-10 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise.
<!-- results:end -->

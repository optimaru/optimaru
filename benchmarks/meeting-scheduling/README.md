# Meeting scheduling benchmark

The [`meeting-scheduling`](../../docs/use-cases/meeting-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `meeting_scheduling_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in external-solver
(`tests/meeting_scheduling/`) keep the two cost definitions identical.

```sh
cd benchmarks/meeting-scheduling/cpsat
OPTIMARU_USE_CASES=meeting_scheduling uv sync --all-groups --all-extras   # also builds this checkout's optimaru (meeting_scheduling only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run meeting-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| small | 8 meetings / 2 meeting rooms / 2 days / 10 people | **0hard/0medium/-336soft** | 0hard/0medium/-344soft |
| medium | 16 meetings / 3 meeting rooms / 3 days / 16 people | **0hard/0medium/-1564soft** | — (no solution in time) |
| demo | 24 meetings / 3 meeting rooms / 4 days / 20 people | **0hard/0medium/-3049soft** | — (no solution in time) |
| large | 48 meetings / 5 meeting rooms / 5 days / 40 people | **0hard/-36medium/-15283soft** | — (no solution in time) |
| xlarge | 96 meetings / 8 meeting rooms / 6 days / 60 people | **0hard/-71medium/-40183soft** | — (no solution in time) |

Measured 2026-11-11 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise.
<!-- results:end -->

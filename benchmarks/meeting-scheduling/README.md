# Meeting scheduling benchmark

The [`meeting-scheduling`](../../docs/use-cases/meeting-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `meeting_scheduling_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in external-solver
(`tests/meeting_scheduling/`) keep the two cost definitions identical.

```sh
cd benchmarks/meeting-scheduling/cpsat
OPTIMARU_USE_CASES=meeting_scheduling uv sync   # also builds this checkout's optimaru (meeting_scheduling only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run meeting-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
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
| small | 8 meetings / 2 meeting rooms / 2 days / 10 people | **0hard/0medium/-336soft** | 0hard/0medium/-339soft |
| small-2 | 8 meetings / 2 meeting rooms / 2 days / 10 people | **0hard/0medium/-430soft** | **0hard/0medium/-430soft** |
| small-3 | 8 meetings / 2 meeting rooms / 2 days / 10 people | 0hard/0medium/-552soft | **0hard/0medium/-545soft** |
| small-4 | 8 meetings / 2 meeting rooms / 2 days / 10 people | **0hard/0medium/-367soft** | **0hard/0medium/-367soft** |
| small-5 | 8 meetings / 2 meeting rooms / 2 days / 10 people | 0hard/-8medium/-899soft | **0hard/-8medium/-871soft** |
| medium | 16 meetings / 3 meeting rooms / 3 days / 16 people | **0hard/0medium/-1564soft** | — (no solution in time) |
| medium-2 | 16 meetings / 3 meeting rooms / 3 days / 16 people | **0hard/0medium/-1977soft** | — (no solution in time) |
| medium-3 | 16 meetings / 3 meeting rooms / 3 days / 16 people | **0hard/0medium/-1929soft** | — (no solution in time) |
| medium-4 | 16 meetings / 3 meeting rooms / 3 days / 16 people | **0hard/-8medium/-3189soft** | — (no solution in time) |
| medium-5 | 16 meetings / 3 meeting rooms / 3 days / 16 people | **0hard/0medium/-1709soft** | — (no solution in time) |
| demo | 24 meetings / 3 meeting rooms / 4 days / 20 people | **0hard/0medium/-3035soft** | — (no solution in time) |
| large | 48 meetings / 5 meeting rooms / 5 days / 40 people | **0hard/-36medium/-15283soft** | — (no solution in time) |
| large-2 | 48 meetings / 5 meeting rooms / 5 days / 40 people | **0hard/-40medium/-12616soft** | — (no solution in time) |
| large-3 | 48 meetings / 5 meeting rooms / 5 days / 40 people | **0hard/-20medium/-12747soft** | — (no solution in time) |
| large-4 | 48 meetings / 5 meeting rooms / 5 days / 40 people | **0hard/0medium/-11215soft** | — (no solution in time) |
| large-5 | 48 meetings / 5 meeting rooms / 5 days / 40 people | **0hard/-16medium/-12732soft** | — (no solution in time) |
| xlarge | 96 meetings / 8 meeting rooms / 6 days / 60 people | **0hard/-71medium/-40183soft** | — (no solution in time) |
| xlarge-2 | 96 meetings / 8 meeting rooms / 6 days / 60 people | **0hard/-153medium/-43598soft** | — (no solution in time) |
| xlarge-3 | 96 meetings / 8 meeting rooms / 6 days / 60 people | **0hard/-63medium/-41776soft** | — (no solution in time) |
| xlarge-4 | 96 meetings / 8 meeting rooms / 6 days / 60 people | **0hard/-90medium/-45668soft** | — (no solution in time) |

Measured 2026-11-12 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

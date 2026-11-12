# Task assigning benchmark

The [`task-assigning`](../../docs/use-cases/task-assigning) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `task_assigning_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; external-solver's parity tests
(`tests/task_assigning/test_solve.py`) keep the two cost definitions identical.

```sh
cd benchmarks/task-assigning/cpsat
OPTIMARU_USE_CASES=task_assigning uv sync   # also builds this checkout's optimaru (task_assigning only)
uv run pytest -q                       # benchmark harness tests
uv run task-assigning-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded. CP-SAT gets `--workers` search workers;
its budget starts once its model is built, and the time the runner reports
for it includes building the model.

On every instance but the five of the smallest size (12 tasks), the CP-SAT
model of this use case is solved one score level at a time, hard first, within
the one budget
([how a CP-SAT run is timed](../README.md#how-a-cp-sat-run-is-timed)).

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| small | 12 tasks / 4 employees / 3 customers | **0hard/0medium/-5871880005292soft** | **0hard/0medium/-5871880005292soft** |
| small-2 | 12 tasks / 4 employees / 3 customers | **0hard/0medium/-4517290004625soft** | **0hard/0medium/-4517290004625soft** |
| small-3 | 12 tasks / 4 employees / 3 customers | **0hard/0medium/-11728890006790soft** | **0hard/0medium/-11728890006790soft** |
| small-4 | 12 tasks / 4 employees / 3 customers | **0hard/0medium/-8217280004479soft** | **0hard/0medium/-8217280004479soft** |
| small-5 | 12 tasks / 4 employees / 3 customers | **0hard/0medium/-27957920010104soft** | **0hard/0medium/-27957920010104soft** |
| demo | 28 tasks / 8 employees / 4 customers | **0hard/0medium/-10179610011485soft** | **0hard/0medium/-10179610011485soft** |
| medium | 60 tasks / 12 employees / 6 customers | **0hard/0medium/-31599610023799soft** | 0hard/0medium/-32029530023986soft |
| medium-2 | 60 tasks / 12 employees / 6 customers | **0hard/0medium/-76601570038110soft** | **0hard/0medium/-76601570038110soft** |
| medium-3 | 60 tasks / 12 employees / 6 customers | **0hard/0medium/-37468660036037soft** | **0hard/0medium/-37468660036037soft** |
| medium-4 | 60 tasks / 12 employees / 6 customers | **0hard/0medium/-16867500023586soft** | 0hard/0medium/-16871140023884soft |
| medium-5 | 60 tasks / 12 employees / 6 customers | **0hard/0medium/-70130690036292soft** | 0hard/0medium/-70336100036621soft |
| large | 120 tasks / 20 employees / 8 customers | **0hard/0medium/-118983210094159soft** | 0hard/0medium/-119613530095683soft |
| large-2 | 120 tasks / 20 employees / 8 customers | **0hard/0medium/-55197810051179soft** | 0hard/0medium/-56581570056887soft |
| large-3 | 120 tasks / 20 employees / 8 customers | **0hard/0medium/-80991870063410soft** | 0hard/0medium/-83166540064612soft |
| large-4 | 120 tasks / 20 employees / 8 customers | **0hard/0medium/-235128250100716soft** | 0hard/0medium/-237664710102487soft |
| large-5 | 120 tasks / 20 employees / 8 customers | **0hard/0medium/-179006150087178soft** | 0hard/0medium/-182183130090933soft |
| xlarge | 240 tasks / 32 employees / 12 customers | **0hard/0medium/-163096040138441soft** | 0hard/0medium/-174832350172427soft |
| xlarge-2 | 240 tasks / 32 employees / 12 customers | **0hard/0medium/-76520100100146soft** | 0hard/0medium/-93406750136690soft |
| xlarge-3 | 240 tasks / 32 employees / 12 customers | **0hard/0medium/-154933870179861soft** | 0hard/0medium/-201973020209875soft |
| xlarge-4 | 240 tasks / 32 employees / 12 customers | **0hard/0medium/-173808550167745soft** | 0hard/0medium/-204677610207523soft |

Measured 2026-11-12 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

# Task assigning benchmark

The [`task-assigning`](../../docs/use-cases/task-assigning) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `task_assigning_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; external-solver's parity tests
(`tests/task_assigning/test_solve.py`) keep the two cost definitions identical.

```sh
cd benchmarks/task-assigning/cpsat
OPTIMARU_USE_CASES=task_assigning uv sync --all-groups --all-extras   # also builds this checkout's optimaru (task_assigning only)
uv run pytest -q                       # benchmark harness tests
uv run task-assigning-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction.

On every instance but the smallest, the CP-SAT model of this use case is
solved one score level at a time, hard first, within the one budget
([how a CP-SAT run is timed](../README.md#how-a-cp-sat-run-is-timed)).

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| small | 12 tasks / 4 employees / 3 customers | **0hard/0medium/-5871880005292soft** | **0hard/0medium/-5871880005292soft** |
| demo | 28 tasks / 8 employees / 4 customers | **0hard/0medium/-10179610011485soft** | **0hard/0medium/-10179610011485soft** |
| medium | 60 tasks / 12 employees / 6 customers | **0hard/0medium/-31599610023799soft** | 0hard/0medium/-31960980024306soft |
| large | 120 tasks / 20 employees / 8 customers | **0hard/0medium/-118983210094159soft** | 0hard/0medium/-120126450097687soft |
| xlarge | 240 tasks / 32 employees / 12 customers | **0hard/0medium/-163096040138441soft** | 0hard/0medium/-176044070161542soft |

Measured 2026-11-11 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

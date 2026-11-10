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

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| demo | demo instance | **0hard/0medium/-10179610011485soft** | **0hard/0medium/-10179610011485soft** |

Measured 2026-11-04 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — treat small differences as noise.
<!-- results:end -->

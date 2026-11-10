# Employee scheduling benchmark

The [`employee-scheduling`](../../docs/use-cases/employee-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `employee_scheduling_cpsat`). The CP-SAT model implements the
same rules under the same names and units, so both answers are
rescored by the engine's own score calculator; parity tests in external-solver
(`tests/employee_scheduling/`) keep the two cost definitions identical.

```sh
cd benchmarks/employee-scheduling/cpsat
OPTIMARU_USE_CASES=employee_scheduling uv sync --all-groups --all-extras   # also builds this checkout's optimaru (employee_scheduling only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run employee-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes, and what the model refuses: [`cpsat/README.md`](cpsat/README.md).

The instances are `optimaru.employee_scheduling.demo.generate_demo_data` at fixed seeds. The
engine runs single-threaded; CP-SAT gets `--workers` search workers and its time
includes model construction.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| demo | 84 shifts / 12 workers / 14 days | **0hard/11520soft** | **0hard/11520soft** |
| medium | 336 shifts / 24 workers / 28 days | **0hard/23040soft** | **0hard/23040soft** |
| large | 672 shifts / 48 workers / 28 days | **0hard/46080soft** | 0hard/42218.5109soft |

Measured 2026-11-10 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise.
Note: unfairness is sqrt(ssd) rounded to 6 significant figures; the CP-SAT objective holds half the squared loads instead, so it can weigh balance against the other soft rules differently from the engine, whose calculator scores both answers.
<!-- results:end -->

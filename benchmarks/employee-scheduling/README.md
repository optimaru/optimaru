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
engine runs single-threaded. CP-SAT gets `--workers` search workers;
its budget starts once its model is built, and the time the runner reports
for it includes building the model.

The largest instance has 80 workers. At 96 CP-SAT rejects the model
(`MODEL_INVALID`: the squared-load objective leaves int64), which is a limit of
the model and not a search result, so the ladder stops below it.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| small | 21 shifts / 6 workers / 7 days | **0hard/4196.1292soft** | **0hard/4196.1292soft** |
| demo | 84 shifts / 12 workers / 14 days | **0hard/11520soft** | **0hard/11520soft** |
| medium | 336 shifts / 24 workers / 28 days | **0hard/23040soft** | **0hard/23040soft** |
| medium-2 | 336 shifts / 24 workers / 28 days | **0hard/23040soft** | **0hard/23040soft** |
| large | 672 shifts / 48 workers / 28 days | **0hard/46080soft** | 0hard/32141.1674soft |
| xlarge | 1120 shifts / 80 workers / 28 days | **0hard/76800soft** | -45242hard/70011.4542soft |

Measured 2026-11-11 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
Note: unfairness is sqrt(ssd) rounded to 6 significant figures; the CP-SAT objective holds half the squared loads instead, so it can weigh balance against the other soft rules differently from the engine, whose calculator scores both answers.
<!-- results:end -->

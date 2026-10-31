# Employee scheduling benchmark

The [`employee-scheduling`](../../use-cases/employee-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `employee_scheduling_cpsat`). The CP-SAT model implements the
same rules under the same names and units, so both answers are
rescored by the engine's own score calculator; parity tests in `cpsat/tests`
keep the two cost definitions identical.

```sh
cd benchmarks/employee-scheduling/cpsat
uv sync --all-groups --all-extras      # builds the engine extension too
uv run pytest -q                       # CP-SAT model + parity tests
uv run employee-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes, and what the model refuses: [`cpsat/README.md`](cpsat/README.md).

The instances are `es_scheduling.demo.generate_demo_data` at fixed seeds. The
engine runs single-threaded; CP-SAT gets `--workers` search workers and its time
includes model construction.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| demo | 84 shifts / 12 workers / 14 days | **0hard/11520soft** | **0hard/11520soft** |
| medium | 336 shifts / 24 workers / 28 days | **0hard/23040soft** | **0hard/23040soft** |
| large | 672 shifts / 48 workers / 28 days | **0hard/46080soft** | -38hard/32101.0928soft |

Measured 2026-10-31 on one Apple Silicon Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — treat small differences as noise.

The soft score of a feasible roster here cannot exceed 960 per worker: two desired days of 480 minutes each. Every 11520, 23040 and 46080 above is that ceiling, so those rosters are optimal. CP-SAT stopped early on demo (0.9 s) and medium (20.6 s) with its own objective proved optimal; the engine has no such proof and uses its whole budget.

CP-SAT's result on large moves a lot between runs. Four 30 s runs at this seed gave -38hard/32101.0928soft (the table), 0hard/23024.7029soft, 0hard/42694.6712soft and 0hard/45089.9706soft. The engine gave 0hard/46080soft in each comparison run.
<!-- results:end -->

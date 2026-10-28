# Maintenance scheduling benchmark

The [`maintenance-scheduling`](../../use-cases/maintenance-scheduling) use case against **OR-Tools CP-SAT** and **Timefold Solver 2.6.0**
([`cpsat/`](cpsat), package `maintenance_scheduling_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in `cpsat/tests`
keep the two cost definitions identical. Timefold solves the use case's
own [quickstart model](../timefold-quickstarts) (v2.6.0, default
configuration); `to_timefold` / `from_timefold` in the runner convert the
instance and its solution, and every solver's solution is also scored by the
quickstart's `ConstraintProvider` as a referee.

```sh
cd benchmarks/maintenance-scheduling/cpsat
uv sync --all-groups --all-extras      # builds the engine extension too
mvn -q -f ../../timefold-quickstarts/pom.xml package   # Timefold models (JDK 21+)
uv run pytest -q                       # CP-SAT model + parity tests
uv run maintenance-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT vs Timefold (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine and Timefold run single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction. Timefold's time includes
JVM start-up.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the **Timefold quickstart's `ConstraintProvider`** (the referee), so all three are judged by the same rules. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT | Timefold 2.6.0 |
| --- | --- | --- | --- | --- |
| small | 14 jobs / 3 crews / 40 dates | 0hard/-71soft | **0hard/-61soft** | 0hard/-63soft |
| large | 48 jobs / 5 crews / 80 dates | 0hard/-3472soft | — (no solution in time) | **0hard/-3467soft** |

Measured 2026-10-29 on one Apple Silicon Mac (16 cores), 30 s per solver and instance, seed 1. The engine and Timefold run single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — treat small differences as noise.
<!-- results:end -->

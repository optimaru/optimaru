# Project job scheduling benchmark

The [`project-job-scheduling`](../../use-cases/project-job-scheduling) use case against **OR-Tools CP-SAT** and **Timefold Solver 2.6.0**
([`cpsat/`](cpsat), package `project_job_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in `cpsat/tests`
keep the two cost definitions identical. Timefold solves the use case's
own [quickstart model](../timefold-quickstarts) (v2.6.0, default
configuration); `to_timefold` / `from_timefold` in the runner convert the
instance and its solution, and every solver's solution is also scored by the
quickstart's `ConstraintProvider` as a referee.

```sh
cd benchmarks/project-job-scheduling/cpsat
uv sync --all-groups --all-extras      # builds the engine extension too
mvn -q -f ../../timefold-quickstarts/pom.xml package   # Timefold models (JDK 21+)
uv run pytest -q                       # CP-SAT model + parity tests
uv run project-job-compare --seconds 10 --workers 8   # engine vs CP-SAT vs Timefold (--markdown, --json)
uv run project-job-public-bench --help              # Patterson RCPSP (Hexaly archive)
```

Public-instance protocol and results: [`cpsat/PUBLIC_BENCHMARKS.md`](cpsat/PUBLIC_BENCHMARKS.md).

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine and Timefold run single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction. Timefold's time includes
JVM start-up.

<!-- results:start -->
## Results

Patterson `Pat1` (32 tasks, 4 resources, critical path 20) through the public runner. Lower makespan is better.

| Solver | Makespan | Resource excess | Time |
| --- | --- | --- | --- |
| engine | 65 | 0 | 30.0 s |
| CP-SAT | **62** | 0 | 18.2 s |

Measured 2026-10-29 on one Apple Silicon Mac (16 cores), 30 s per solver and instance. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — a snapshot, not a statistical study.
<!-- results:end -->

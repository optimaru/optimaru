# Project job scheduling benchmark

The [`project-job-scheduling`](../../use-cases/project-job-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `project_job_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in `cpsat/tests`
keep the two cost definitions identical.

```sh
cd benchmarks/project-job-scheduling/cpsat
uv sync --all-groups --all-extras      # builds the engine extension too
uv run pytest -q                       # CP-SAT model + parity tests
uv run project-job-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
uv run project-job-public-bench --help              # Patterson RCPSP (Hexaly archive)
```

Public-instance protocol and results: [`cpsat/PUBLIC_BENCHMARKS.md`](cpsat/PUBLIC_BENCHMARKS.md).

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction.


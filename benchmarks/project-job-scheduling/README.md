# Project job scheduling benchmark

Transcribes the Timefold Quickstarts `project-job-scheduling` example's
demo data generator (Apache-2.0), with modifications; see the repository [NOTICE](../../NOTICE).

The [`project-job-scheduling`](../../use-cases/project-job-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `project_job_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in external-solver
(`tests/project_job_scheduling/`) keep the two cost definitions identical.

```sh
cd benchmarks/project-job-scheduling/cpsat
uv sync --all-groups --all-extras      # builds the engine extension too
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run project-job-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
uv run project-job-public-bench --help              # RG30 RCPSP instances (Patterson format)
```

Public-instance protocol and results: [`cpsat/PUBLIC_BENCHMARKS.md`](cpsat/PUBLIC_BENCHMARKS.md).

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction.


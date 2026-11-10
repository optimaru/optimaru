# Project job scheduling benchmark

Transcribes the Timefold Quickstarts `project-job-scheduling` example's
demo data generator (Apache-2.0), with modifications; see the repository [NOTICE](../../NOTICE).

The [`project-job-scheduling`](../../docs/use-cases/project-job-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `project_job_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in external-solver
(`tests/project_job_scheduling/`) keep the two cost definitions identical.

```sh
cd benchmarks/project-job-scheduling/cpsat
OPTIMARU_USE_CASES=project_job_scheduling uv sync --all-groups --all-extras   # also builds this checkout's optimaru (project_job_scheduling only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run project-job-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
uv run project-job-public-bench --help              # RG30 RCPSP instances (Patterson format)
```

Public-instance protocol and results: [`cpsat/PUBLIC_BENCHMARKS.md`](cpsat/PUBLIC_BENCHMARKS.md).

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| small | 20 jobs / 2 projects / 7 res | **0hard/0medium/-10soft** | **0hard/0medium/-10soft** |
| demo | 24 jobs / 2 projects / 7 res | **0hard/0medium/-7soft** | **0hard/0medium/-7soft** |
| medium | 72 jobs / 4 projects / 13 res | **0hard/-9medium/-20soft** | **0hard/-9medium/-20soft** |
| medium-2 | 72 jobs / 4 projects / 13 res | **0hard/0medium/-18soft** | **0hard/0medium/-18soft** |
| large | 156 jobs / 6 projects / 19 res | **0hard/-1medium/-35soft** | 0hard/-1medium/-55soft |
| xlarge | 300 jobs / 10 projects / 31 res | **0hard/-11medium/-61soft** | — (no solution in time) |

Measured 2026-11-11 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise.
<!-- results:end -->

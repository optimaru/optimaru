# Conference scheduling benchmark

The [`conference-scheduling`](../../docs/use-cases/conference-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `conference_scheduling_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in external-solver
(`tests/conference_scheduling/`) keep the two cost definitions identical.

```sh
cd benchmarks/conference-scheduling/cpsat
OPTIMARU_USE_CASES=conference_scheduling uv sync --all-groups --all-extras   # also builds this checkout's optimaru (conference_scheduling only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run conference-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded. CP-SAT gets `--workers` search workers;
its budget starts once its model is built, and the time the runner reports
for it includes building the model.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| demo | 15 talks / 5 halls / 6 timeslots / 12 speakers | **0hard/-57990soft** | **0hard/-57990soft** |
| days2 | 30 talks / 5 halls / 12 timeslots / 24 speakers | **0hard/-288045soft** | 0hard/-482775soft |
| days3 | 70 talks / 8 halls / 18 timeslots / 56 speakers | **0hard/-2108280soft** | — (no solution in time) |
| days4 | 118 talks / 10 halls / 24 timeslots / 94 speakers | **0hard/-5382840soft** | — (no solution in time) |
| days5 | 190 talks / 13 halls / 30 timeslots / 152 speakers | **0hard/-15019890soft** | — (no solution in time) |

Measured 2026-11-11 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

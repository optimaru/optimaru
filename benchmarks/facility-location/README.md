# Facility location benchmark

Transcribes the Timefold Quickstarts `facility-location` example's demo data generator (Apache-2.0),
with modifications; see the repository [NOTICE](../../NOTICE).

The [`facility-location`](../../docs/use-cases/facility-location) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `facility_location_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in external-solver
(`tests/facility_location/`) keep the two cost definitions identical.

```sh
cd benchmarks/facility-location/cpsat
OPTIMARU_USE_CASES=facility_location uv sync --all-groups --all-extras   # also builds this checkout's optimaru (facility_location only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run facility-location-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| small | 30 cons / 15 fac | **0hard/-848812soft** | **0hard/-848812soft** |
| demo | 60 cons / 30 fac | **0hard/-1343284soft** | **0hard/-1343284soft** |
| medium | 200 cons / 50 fac | 0hard/-3144802soft | **0hard/-3099950soft** |
| medium-2 | 200 cons / 50 fac | 0hard/-3246180soft | **0hard/-3190564soft** |
| large | 600 cons / 80 fac | 0hard/-6498876soft | **0hard/-6381914soft** |
| xlarge | 1200 cons / 120 fac | 0hard/-10319278soft | **0hard/-9988242soft** |

Measured 2026-11-11 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

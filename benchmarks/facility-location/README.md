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
OPTIMARU_USE_CASES=facility_location uv sync   # also builds this checkout's optimaru (facility_location only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run facility-location-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
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
| small | 30 cons / 15 fac | **0hard/-848812soft** | **0hard/-848812soft** |
| small-2 | 30 cons / 15 fac | **0hard/-908092soft** | **0hard/-908092soft** |
| small-3 | 30 cons / 15 fac | **0hard/-886858soft** | **0hard/-886858soft** |
| small-4 | 30 cons / 15 fac | **0hard/-872352soft** | **0hard/-872352soft** |
| demo | 60 cons / 30 fac | **0hard/-1343284soft** | **0hard/-1343284soft** |
| demo-2 | 60 cons / 30 fac | **0hard/-1369766soft** | **0hard/-1369766soft** |
| demo-3 | 60 cons / 30 fac | **0hard/-1424026soft** | **0hard/-1424026soft** |
| demo-4 | 60 cons / 30 fac | **0hard/-1477750soft** | **0hard/-1477750soft** |
| medium | 200 cons / 50 fac | 0hard/-3105800soft | **0hard/-3099950soft** |
| medium-2 | 200 cons / 50 fac | **0hard/-3190564soft** | **0hard/-3190564soft** |
| medium-3 | 200 cons / 50 fac | 0hard/-3086642soft | **0hard/-3083936soft** |
| medium-4 | 200 cons / 50 fac | 0hard/-3114582soft | **0hard/-3106666soft** |
| medium-5 | 200 cons / 50 fac | **0hard/-3195642soft** | **0hard/-3195642soft** |
| large | 600 cons / 80 fac | 0hard/-6383234soft | **0hard/-6381914soft** |
| large-2 | 600 cons / 80 fac | 0hard/-6385956soft | **0hard/-6367404soft** |
| large-3 | 600 cons / 80 fac | 0hard/-6422672soft | **0hard/-6422198soft** |
| large-4 | 600 cons / 80 fac | 0hard/-6414198soft | **0hard/-6408496soft** |
| xlarge | 1200 cons / 120 fac | 0hard/-10017572soft | **0hard/-9988242soft** |
| xlarge-2 | 1200 cons / 120 fac | 0hard/-10276842soft | **0hard/-10266268soft** |
| xlarge-3 | 1200 cons / 120 fac | 0hard/-10101004soft | **0hard/-10092708soft** |

Measured 2026-11-12 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

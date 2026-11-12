# Flight crew scheduling benchmark

Transcribes the Timefold Quickstarts `flight-crew-scheduling` example's
demo data generator (Apache-2.0), with modifications; see the repository [NOTICE](../../NOTICE).

The [`flight-crew-scheduling`](../../docs/use-cases/flight-crew-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `flight_crew_scheduling_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in external-solver
(`tests/flight_crew_scheduling/`) keep the two cost definitions identical.

```sh
cd benchmarks/flight-crew-scheduling/cpsat
OPTIMARU_USE_CASES=flight_crew_scheduling uv sync   # also builds this checkout's optimaru (flight_crew_scheduling only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run flight-crew-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
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
| small | 32 seats / 32 crew / 8 flights | **0hard/-20000soft** | **0hard/-20000soft** |
| small-2 | 34 seats / 42 crew / 8 flights | **0hard/-22000soft** | **0hard/-22000soft** |
| small-3 | 38 seats / 26 crew / 8 flights | -3hard/-27000soft | **-3hard/-26000soft** |
| small-4 | 34 seats / 42 crew / 8 flights | **0hard/-12000soft** | **0hard/-12000soft** |
| demo | 62 seats / 50 crew / 14 flights | **0hard/-23000soft** | **0hard/-23000soft** |
| demo-2 | 60 seats / 42 crew / 14 flights | **0hard/-12000soft** | **0hard/-12000soft** |
| demo-3 | 62 seats / 50 crew / 14 flights | **0hard/-17000soft** | **0hard/-17000soft** |
| demo-4 | 58 seats / 50 crew / 14 flights | **0hard/-8000soft** | **0hard/-8000soft** |
| medium | 120 seats / 50 crew / 28 flights | **-2hard/-25000soft** | -33hard/-56000soft |
| medium-2 | 122 seats / 50 crew / 28 flights | **0hard/-23000soft** | 0hard/-25000soft |
| medium-3 | 120 seats / 50 crew / 28 flights | **0hard/-49000soft** | -16hard/-45000soft |
| medium-4 | 118 seats / 50 crew / 28 flights | **0hard/-23000soft** | -230hard/-48000soft |
| medium-5 | 118 seats / 50 crew / 28 flights | **0hard/-23000soft** | -377hard/-44000soft |
| large | 236 seats / 50 crew / 56 flights | **-21hard/-35000soft** | -41360hard/-7000soft |
| large-2 | 244 seats / 50 crew / 56 flights | **-20hard/-27000soft** | -48771hard/-12000soft |
| large-3 | 240 seats / 50 crew / 56 flights | **-16hard/-30000soft** | -38740hard/-7000soft |
| large-4 | 252 seats / 50 crew / 56 flights | **-18hard/-26000soft** | -45670hard/-13000soft |
| xlarge | 480 seats / 50 crew / 112 flights | **-113hard/-36000soft** | — (no solution in time) |
| xlarge-2 | 478 seats / 50 crew / 112 flights | **-53hard/-46000soft** | — (no solution in time) |
| xlarge-3 | 484 seats / 50 crew / 112 flights | **-95hard/-24000soft** | — (no solution in time) |

Measured 2026-11-12 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

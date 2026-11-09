# Flight crew scheduling benchmark

Transcribes the Timefold Quickstarts `flight-crew-scheduling` example's demo data generator (Apache-2.0),
with modifications; see the repository [NOTICE](../../NOTICE).

The [`flight-crew-scheduling`](../../use-cases/flight-crew-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `flight_crew_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in `cpsat/tests`
keep the two cost definitions identical.

```sh
cd benchmarks/flight-crew-scheduling/cpsat
uv sync --all-groups --all-extras      # builds the engine extension too
uv run pytest -q                       # CP-SAT model + parity tests
uv run flight-crew-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| demo | 62 seats / 50 crew / 14 flights | **0hard/-23000soft** | **0hard/-23000soft** |
| medium | 120 seats / 50 crew / 28 flights | **-2hard/-25000soft** | -11hard/-50000soft |
| large | 236 seats / 50 crew / 56 flights | **-21hard/-35000soft** | -41471hard/-7000soft |

Measured 2026-11-04 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — treat small differences as noise.
<!-- results:end -->

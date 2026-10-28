# Flight crew scheduling benchmark

The [`flight-crew-scheduling`](../../use-cases/flight-crew-scheduling) use case against **OR-Tools CP-SAT** and **Timefold Solver 2.6.0**
([`cpsat/`](cpsat), package `flight_crew_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in `cpsat/tests`
keep the two cost definitions identical. Timefold solves the use case's
own [quickstart model](../timefold-quickstarts) (v2.6.0, default
configuration); `to_timefold` / `from_timefold` in the runner convert the
instance and its solution, and every solver's solution is also scored by the
quickstart's `ConstraintProvider` as a referee.

```sh
cd benchmarks/flight-crew-scheduling/cpsat
uv sync --all-groups --all-extras      # builds the engine extension too
mvn -q -f ../../timefold-quickstarts/pom.xml package   # Timefold models (JDK 21+)
uv run pytest -q                       # CP-SAT model + parity tests
uv run flight-crew-compare --seconds 10 --workers 8   # engine vs CP-SAT vs Timefold (--markdown, --json)
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
| demo | 62 seats / 50 crew / 14 flights | **0hard/-23000soft** | **0hard/-23000soft** | 0hard/-38000soft |
| medium | 120 seats / 50 crew / 28 flights | **-2hard/-25000soft** | -21hard/-48000soft | -2hard/-46000soft |
| large | 236 seats / 50 crew / 56 flights | **-21hard/-35000soft** | -41311hard/-7000soft | -21hard/-49000soft |

Measured 2026-10-29 on one Apple Silicon Mac (16 cores), 30 s per solver and instance, seed 1. The engine and Timefold run single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — treat small differences as noise.
<!-- results:end -->

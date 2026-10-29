# Tournament scheduling benchmark

The [`tournament-scheduling`](../../use-cases/tournament-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `tournament_scheduling_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in `cpsat/tests`
keep the two cost definitions identical.

```sh
cd benchmarks/tournament-scheduling/cpsat
uv sync --all-groups --all-extras      # builds the engine extension too
uv run pytest -q                       # CP-SAT model + parity tests
uv run tournament-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| demo | 72 slots / 7 teams / 18 days | **0hard/0medium/-1.54919soft** | **0hard/0medium/-1.54919soft** |
| large | 180 slots / 10 teams / 36 days | **0hard/0medium/0soft** | 0hard/0medium/-2soft |
| xlarge | 800 slots / 20 teams / 100 days | **0hard/0medium/-6.06977soft** | -981hard/-60.4318medium/-109.639soft |

Measured 2026-10-29 on one Apple Silicon Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — treat small differences as noise.
<!-- results:end -->

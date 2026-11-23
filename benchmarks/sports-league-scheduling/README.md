# Sports league scheduling benchmark

The [`sports-league-scheduling`](../../docs/use-cases/sports-league-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `sports_league_scheduling_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; external-solver's parity tests
(`tests/sports_league_scheduling/test_solve.py`) keep the two cost definitions identical.

```sh
cd benchmarks/sports-league-scheduling/cpsat
OPTIMARU_USE_CASES=sports_league_scheduling uv sync   # also builds this checkout's optimaru (sports_league_scheduling only)
uv run pytest -q                       # benchmark harness tests
uv run sports-league-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
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
| teams6 | 6 teams / 12 rounds / 30 matches | 0hard/-45462soft | **0hard/-43768soft** |
| teams6-2 | 6 teams / 12 rounds / 30 matches | **0hard/-31763soft** | 0hard/-33223soft |
| teams6-3 | 6 teams / 12 rounds / 30 matches | 0hard/-30447soft | **0hard/-29980soft** |
| teams6-4 | 6 teams / 12 rounds / 30 matches | 0hard/-46846soft | **0hard/-44657soft** |
| teams8 | 8 teams / 17 rounds / 56 matches | **0hard/-63293soft** | 0hard/-67281soft |
| teams8-2 | 8 teams / 17 rounds / 56 matches | **0hard/-59718soft** | 0hard/-62796soft |
| teams8-3 | 8 teams / 17 rounds / 56 matches | **0hard/-62588soft** | 0hard/-66194soft |
| teams8-4 | 8 teams / 17 rounds / 56 matches | **0hard/-65639soft** | 0hard/-72484soft |
| teams10 | 10 teams / 22 rounds / 90 matches | **0hard/-94525soft** | 0hard/-107672soft |
| teams10-2 | 10 teams / 22 rounds / 90 matches | **0hard/-79809soft** | 0hard/-88329soft |
| teams10-3 | 10 teams / 22 rounds / 90 matches | **0hard/-94465soft** | 0hard/-103402soft |
| teams10-4 | 10 teams / 22 rounds / 90 matches | **0hard/-92077soft** | 0hard/-102263soft |
| teams12 | 12 teams / 27 rounds / 132 matches | **0hard/-136562soft** | -2hard/-211007soft |
| teams12-2 | 12 teams / 27 rounds / 132 matches | **0hard/-118196soft** | 0hard/-196813soft |
| teams12-3 | 12 teams / 27 rounds / 132 matches | **0hard/-114973soft** | 0hard/-176808soft |
| teams12-4 | 12 teams / 27 rounds / 132 matches | **0hard/-137585soft** | -1hard/-188111soft |
| demo | 14 teams / 32 rounds / 182 matches | **0hard/-100352soft** | -12hard/-228755soft |
| teams18 | 18 teams / 42 rounds / 306 matches | **0hard/-259670soft** | -208hard/-401092soft |
| teams18-2 | 18 teams / 42 rounds / 306 matches | **0hard/-229544soft** | -190hard/-311290soft |
| teams18-3 | 18 teams / 42 rounds / 306 matches | **0hard/-264908soft** | 0hard/-510943soft |

Measured 2026-11-24 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

# Sports league scheduling benchmark

The [`sports-league-scheduling`](../../docs/use-cases/sports-league-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `sports_league_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; external-solver's parity tests
(`tests/sports_league_scheduling/test_solve.py`) keep the two cost definitions identical.

```sh
cd benchmarks/sports-league-scheduling/cpsat
OPTIMARU_USE_CASES=sports_league_scheduling uv sync --all-groups --all-extras   # also builds this checkout's optimaru (sports_league_scheduling only)
uv run pytest -q                       # benchmark harness tests
uv run sports-league-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| teams6 | 6 teams / 12 rounds / 30 matches | 0hard/-49266soft | **0hard/-44281soft** |
| teams8 | 8 teams / 17 rounds / 56 matches | 0hard/-69745soft | **0hard/-63949soft** |
| teams10 | 10 teams / 22 rounds / 90 matches | 0hard/-109004soft | **0hard/-107733soft** |
| demo | 14 teams / 32 rounds / 182 matches | **0hard/-110864soft** | -7hard/-222011soft |
| teams18 | 18 teams / 42 rounds / 306 matches | **0hard/-287821soft** | -208hard/-401092soft |

Measured 2026-11-11 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise.
<!-- results:end -->

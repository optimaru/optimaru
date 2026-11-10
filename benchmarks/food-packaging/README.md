# Food packaging benchmark

The [`food-packaging`](../../docs/use-cases/food-packaging) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `food_packaging_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in external-solver
(`tests/food_packaging/`) keep the two cost definitions identical.

```sh
cd benchmarks/food-packaging/cpsat
OPTIMARU_USE_CASES=food_packaging uv sync --all-groups --all-extras   # also builds this checkout's optimaru (food_packaging only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run food-packaging-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| lines2 | 2 lines / 20 jobs | **0hard/0medium/-11010632soft** | 0hard/0medium/-11108353soft |
| lines3 | 3 lines / 40 jobs | **0hard/0medium/-26950083soft** | 0hard/-6896medium/-1163193soft |
| demo | 5 lines / 100 jobs | **0hard/0medium/-96495547soft** | — (no solution in time) |
| lines8 | 8 lines / 200 jobs | **0hard/0medium/-234142881soft** | — (no solution in time) |
| lines10 | 10 lines / 300 jobs | **0hard/0medium/-425491369soft** | — (no solution in time) |

Measured 2026-11-11 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise.
<!-- results:end -->

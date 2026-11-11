# Food packaging benchmark

The [`food-packaging`](../../docs/use-cases/food-packaging) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `food_packaging_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in external-solver
(`tests/food_packaging/`) keep the two cost definitions identical.

```sh
cd benchmarks/food-packaging/cpsat
OPTIMARU_USE_CASES=food_packaging uv sync   # also builds this checkout's optimaru (food_packaging only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run food-packaging-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded. CP-SAT gets `--workers` search workers;
its budget starts once its model is built, and the time the runner reports
for it includes building the model.

The CP-SAT model of this use case is solved one score level at a time, hard
first, within the one budget
([how a CP-SAT run is timed](../README.md#how-a-cp-sat-run-is-timed)). On the
4-line plant the hard level finds its first solution, the empty plan, after
about 16 s, past its 10 s share of the budget, and the medium level, which
places the jobs, finds nothing better in its share (about 5 s: half of what is
left once the hard level's solution has been written into the model as a
hint): CP-SAT's answer there leaves every job unassigned (`-12914medium` is
the 12,914 minutes of the 60 jobs, and `0soft` an empty plan). On the demo
plant the hard level finds no solution in its 30 s.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| lines2 | 2 lines / 20 jobs | **0hard/0medium/-10977133soft** | 0hard/0medium/-11054404soft |
| lines3 | 3 lines / 40 jobs | **0hard/0medium/-26711524soft** | 0hard/-6786medium/-2419682soft |
| lines4 | 4 lines / 60 jobs | **0hard/0medium/-46407583soft** | 0hard/-12914medium/0soft |
| demo | 5 lines / 100 jobs | **0hard/0medium/-96189767soft** | — (no solution in time) |
| lines8 | 8 lines / 200 jobs | **0hard/0medium/-233424529soft** | — (no solution in time) |
| lines10 | 10 lines / 300 jobs | **0hard/0medium/-424357320soft** | — (no solution in time) |

Measured 2026-11-12 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

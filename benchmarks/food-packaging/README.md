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
([how a CP-SAT run is timed](../README.md#how-a-cp-sat-run-is-timed)). In five
rows of the table (3 lines / 60 jobs, the three 4-line plants and 5 lines / 50
jobs) CP-SAT's answer leaves every job unassigned: `0soft` is an empty plan,
and the medium level is the minutes of the jobs (`-12914medium` is the 12,914
minutes of the first 60 jobs). How that comes about was measured on the 4-line
plant of 60 jobs on 2026-11-11: the hard level finds its first solution, the
empty plan, after about 16 s, past its 10 s share of the budget, and the
medium level, which places the jobs, finds nothing better in its share (about
5 s: half of what is left once the hard level's solution has been written into
the model as a hint). In three smaller rows (2 lines / 30 jobs, 3 lines / 30
and 40 jobs) the medium level placed some of the jobs and left the others out.
On four plants, from 5 lines / 75 jobs to 6 lines / 120 jobs, the hard level
finds no solution in its 30 s, and the runs of the five largest plants are
stopped 60 s past the budget.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| lines2x10 | 2 lines / 10 jobs | 0hard/0medium/-2933284soft | **0hard/0medium/-2930626soft** |
| lines2 | 2 lines / 20 jobs | **0hard/0medium/-10977133soft** | 0hard/0medium/-11199501soft |
| lines2x30 | 2 lines / 30 jobs | **0hard/0medium/-23751029soft** | 0hard/-1611medium/-15830618soft |
| lines3x20 | 3 lines / 20 jobs | **0hard/0medium/-7250891soft** | 0hard/0medium/-7858659soft |
| lines3x30 | 3 lines / 30 jobs | **0hard/0medium/-15703457soft** | 0hard/-3072medium/-6310706soft |
| lines3 | 3 lines / 40 jobs | **0hard/0medium/-26711524soft** | 0hard/-6786medium/-2419682soft |
| lines3x60 | 3 lines / 60 jobs | **0hard/0medium/-62674394soft** | 0hard/-12914medium/0soft |
| lines4x40 | 4 lines / 40 jobs | **0hard/0medium/-19894044soft** | 0hard/-8443medium/0soft |
| lines4 | 4 lines / 60 jobs | **0hard/0medium/-46407583soft** | 0hard/-12914medium/0soft |
| lines4x80 | 4 lines / 80 jobs | **0hard/0medium/-79272037soft** | 0hard/-16978medium/0soft |
| lines5x50 | 5 lines / 50 jobs | **0hard/0medium/-24787729soft** | 0hard/-10573medium/0soft |
| lines5x75 | 5 lines / 75 jobs | **0hard/0medium/-56000213soft** | — (no solution in time) |
| demo | 5 lines / 100 jobs | **0hard/0medium/-96189767soft** | — (no solution in time) |
| lines6x90 | 6 lines / 90 jobs | **0hard/0medium/-64628549soft** | — (no solution in time) |
| lines6x120 | 6 lines / 120 jobs | **0hard/0medium/-117158749soft** | — (no solution in time) |
| lines7x140 | 7 lines / 140 jobs | **0hard/0medium/-134304778soft** | — (no solution in time) |
| lines8x150 | 8 lines / 150 jobs | **0hard/0medium/-134281075soft** | — (no solution in time) |
| lines8 | 8 lines / 200 jobs | **0hard/0medium/-233451581soft** | — (no solution in time) |
| lines9x250 | 9 lines / 250 jobs | **0hard/0medium/-326613955soft** | — (no solution in time) |
| lines10 | 10 lines / 300 jobs | **0hard/0medium/-424357320soft** | — (no solution in time) |

Measured 2026-11-12 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

# Semiconductor scheduling benchmark

The [`semiconductor-scheduling`](../../docs/use-cases/semiconductor-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `semiconductor_scheduling_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, as external-solver plugins of the domain
specs, so both answers are rescored by the use case's own C++ evaluator; external-solver's
parity tests (`tests/semiconductor_scheduling/test_parity.py`) keep the two cost definitions
identical (formulation notes:
[`cpsat/README.md`](cpsat/README.md)).

```sh
cd benchmarks/semiconductor-scheduling/cpsat
OPTIMARU_USE_CASES=semiconductor_scheduling uv sync   # also builds this checkout's optimaru (semiconductor_scheduling only)
uv run pytest -q                       # benchmark harness tests
uv run semiconductor-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT table (--markdown, --json)
```

The engine runs single-threaded. CP-SAT gets `--workers` search workers;
its budget starts once its model is built, and the time the runner reports
for it includes building the model.

The engine's result on the larger instances varies between runs of the same
build: four 30 s runs of the 18-lot instance on 2026-11-11 gave soft -839, -842,
-881 and -894 (the 6-lot instance repeats to the digit), the table of
2026-11-12 had -843, and the run in the table below gave -841. CP-SAT's result on
that instance moves too (-857 on 2026-11-11, -838 on 2026-11-12, -834 here). Read
a margin of a few percent in this table as noise.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| lots6 | 6 lots / 6 machines | **0hard/-163soft** | **0hard/-163soft** |
| lots6-2 | 6 lots / 6 machines | **0hard/-166soft** | **0hard/-166soft** |
| lots6-3 | 6 lots / 6 machines | **0hard/-124soft** | **0hard/-124soft** |
| lots6-4 | 6 lots / 6 machines | **0hard/-151soft** | **0hard/-151soft** |
| lots12 | 12 lots / 6 machines | **0hard/-415soft** | **0hard/-415soft** |
| lots12-2 | 12 lots / 6 machines | **0hard/-400soft** | 0hard/-407soft |
| lots12-3 | 12 lots / 6 machines | **0hard/-369soft** | 0hard/-373soft |
| lots12-4 | 12 lots / 6 machines | 0hard/-364soft | **0hard/-361soft** |
| lots18 | 18 lots / 6 machines | 0hard/-841soft | **0hard/-834soft** |
| lots18-2 | 18 lots / 6 machines | 0hard/-898soft | **0hard/-873soft** |
| lots18-3 | 18 lots / 6 machines | 0hard/-831soft | **0hard/-810soft** |
| lots18-4 | 18 lots / 6 machines | 0hard/-638soft | **0hard/-624soft** |
| lots24 | 24 lots / 6 machines | **0hard/-2161soft** | 0hard/-2374soft |
| lots24-2 | 24 lots / 6 machines | **0hard/-1741soft** | 0hard/-1802soft |
| lots24-3 | 24 lots / 6 machines | **0hard/-2891soft** | 0hard/-3102soft |
| lots36 | 36 lots / 6 machines | **0hard/-10589soft** | 0hard/-12217soft |
| lots36-2 | 36 lots / 6 machines | **0hard/-8274soft** | 0hard/-9677soft |
| lots36-3 | 36 lots / 6 machines | **0hard/-10729soft** | 0hard/-11618soft |
| lots48 | 48 lots / 6 machines | **0hard/-24441soft** | 0hard/-26191soft |
| lots48-2 | 48 lots / 6 machines | **0hard/-17922soft** | 0hard/-19651soft |

Measured 2026-11-24 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

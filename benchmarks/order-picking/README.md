# Order picking benchmark

The [`order-picking`](../../docs/use-cases/order-picking) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `order_picking_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in external-solver
(`tests/order_picking/`) keep the two cost definitions identical.

```sh
cd benchmarks/order-picking/cpsat
OPTIMARU_USE_CASES=order_picking uv sync   # also builds this checkout's optimaru (order_picking only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run order-picking-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The instances are in the default warehouse (5 columns x 3 shelving rows), the only layout the
CP-SAT model knows: a request with another layout (`warehouse`, or `locations` and `distances`,
see the [guide](../../docs/use-cases/order-picking/README.md#warehouse-layout)) solves on the
engine only until external-solver follows.

The engine runs single-threaded. CP-SAT gets `--workers` search workers;
its budget starts once its model is built, and the time the runner reports
for it includes building the model.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| tiny | 10 picks / 1 trolley | **0hard/-4134soft** | **0hard/-4134soft** |
| tiny-2 | 15 picks / 1 trolley | **0hard/-4124soft** | **0hard/-4124soft** |
| tiny-3 | 17 picks / 1 trolley | **0hard/-4162soft** | **0hard/-4162soft** |
| tiny-4 | 16 picks / 1 trolley | **0hard/-4154soft** | **0hard/-4154soft** |
| small | 22 picks / 2 trolleys | **0hard/-8244soft** | **0hard/-8244soft** |
| small-2 | 18 picks / 2 trolleys | **0hard/-8210soft** | **0hard/-8210soft** |
| small-3 | 15 picks / 2 trolleys | **0hard/-8218soft** | **0hard/-8218soft** |
| small-4 | 27 picks / 2 trolleys | **0hard/-8258soft** | **0hard/-8258soft** |
| medium | 56 picks / 4 trolleys | **0hard/-20478soft** | 0hard/-20488soft |
| medium-2 | 71 picks / 4 trolleys | **0hard/-20518soft** | 0hard/-20538soft |
| medium-3 | 69 picks / 4 trolleys | **0hard/-20514soft** | 0hard/-20552soft |
| medium-4 | 62 picks / 4 trolleys | **0hard/-20524soft** | 0hard/-20532soft |
| large | 137 picks / 8 trolleys | **0hard/-40940soft** | 0hard/-41140soft |
| large-2 | 138 picks / 8 trolleys | **0hard/-40908soft** | 0hard/-41130soft |
| large-3 | 106 picks / 8 trolleys | **0hard/-40842soft** | 0hard/-40920soft |
| large-4 | 120 picks / 8 trolleys | **0hard/-40888soft** | 0hard/-41044soft |
| xlarge | 235 picks / 16 trolleys | **0hard/-81604soft** | -40hard/-99200soft |
| xlarge-2 | 227 picks / 16 trolleys | **0hard/-81552soft** | 0hard/-92198soft |
| xlarge-3 | 224 picks / 16 trolleys | **0hard/-81688soft** | 0hard/-94282soft |
| xlarge-4 | 233 picks / 16 trolleys | **0hard/-81670soft** | 0hard/-94472soft |

Measured 2026-11-24 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

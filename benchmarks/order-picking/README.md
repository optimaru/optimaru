# Order picking benchmark

The [`order-picking`](../../docs/use-cases/order-picking) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `order_picking_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in external-solver
(`tests/order_picking/`) keep the two cost definitions identical.

```sh
cd benchmarks/order-picking/cpsat
OPTIMARU_USE_CASES=order_picking uv sync --all-groups --all-extras   # also builds this checkout's optimaru (order_picking only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run order-picking-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded; CP-SAT gets `--workers` search
workers and its time includes model construction.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| small | 22 picks / 2 trolleys | **0hard/-8244soft** | **0hard/-8244soft** |
| medium | 56 picks / 4 trolleys | **0hard/-20478soft** | 0hard/-20488soft |

Measured 2026-11-04 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — treat small differences as noise.
<!-- results:end -->

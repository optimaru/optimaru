# Pickup delivery benchmark

The [`pickup-delivery`](../../docs/use-cases/pickup-delivery) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `pickup_delivery_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in external-solver
(`tests/pickup_delivery/`) keep the two cost definitions identical.

```sh
cd benchmarks/pickup-delivery/cpsat
OPTIMARU_USE_CASES=pickup_delivery uv sync --all-groups --all-extras   # also builds this checkout's optimaru (pickup_delivery only)
uv run pytest -q                       # comparison smoke tests (model tests: external-solver)
uv run pickup-delivery-public-bench --help              # Li & Lim PDPTW (SINTEF TOP archive)
uv run pickup-delivery-public-bench --synthetic 200 --seconds 30 --workers 8   # seeded synthetic instance
```

Public-instance protocol and results: [`cpsat/PUBLIC_BENCHMARKS.md`](cpsat/PUBLIC_BENCHMARKS.md).

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded; CP-SAT gets `--workers` search workers and
its time includes model construction.

<!-- results:start -->
## Results

**Synthetic Li & Lim-shaped instances** (`--synthetic N`, seed 0): customers in a
100 x 100 square, half in six clusters, service time 90, one vehicle per three requests. Cells
are vehicles / distance.

| Requests | engine | CP-SAT |
| --- | --- | --- |
| 25 | **8 / 1,636.5** | 8 / 1,653.3 |
| 50 | **13 / 3,369.6** | incomplete (leaves stops unserved) |
| 100 | **22 / 5,615.2** | no solution in time |
| 200 | **36 / 10,509.9** | no solution in time |
| 400 | 51 / 19,982 | not run |

CP-SAT is not run at 400 requests: its model takes minutes and gigabytes to build (at 400 requests it was still being built after 10 minutes at 11 GB, 2026-10-29).

The Li & Lim results are in [`cpsat/PUBLIC_BENCHMARKS.md`](cpsat/PUBLIC_BENCHMARKS.md).

Measured 2026-11-11 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 0. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — a snapshot, not a statistical study.
<!-- results:end -->

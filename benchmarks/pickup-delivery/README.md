# Pickup delivery benchmark

The [`pickup-delivery`](../../docs/use-cases/pickup-delivery) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `pickup_delivery_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator. The model is kept here
(`cpsat/pickup_delivery_cpsat/model`, external-solver v0.3.0's ported to the 0.6 input), and
the parity tests in `cpsat/tests/` (external-solver v0.3.0's `test_constraint_parity.py` and
`test_solve.py`, ported, and `test_model.py` for the 0.6 input) keep the two cost definitions identical.

```sh
cd benchmarks/pickup-delivery/cpsat
OPTIMARU_USE_CASES=pickup_delivery uv sync   # also builds this checkout's optimaru (pickup_delivery only)
uv run pytest -q                       # model parity and comparison smoke tests
uv run pickup-delivery-public-bench --help              # Li & Lim PDPTW (SINTEF TOP archive)
uv run pickup-delivery-public-bench --synthetic 200 --seconds 30 --workers 8   # seeded synthetic instance
```

The synthetic table below: `--synthetic N` repeats, and `--synthetic-seeds` (comma-separated,
default the value of `--seed`) gives one instance per size and seed:

```sh
uv run pickup-delivery-public-bench --synthetic 10 --synthetic 15 --synthetic 20 --synthetic 25 \
  --synthetic 30 --synthetic 40 --synthetic 50 --synthetic 75 --synthetic 100 --synthetic 200 \
  --synthetic-seeds 0,1 --seconds 30 --workers 8
uv run pickup-delivery-public-bench --synthetic 400 --synthetic-seeds 0 --seconds 30 --backend engine
```

Public-instance protocol and results: [`cpsat/PUBLIC_BENCHMARKS.md`](cpsat/PUBLIC_BENCHMARKS.md).

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded. CP-SAT gets `--workers` search workers;
its budget starts once its model is built, and the time the runner reports
for it includes building the model.

CP-SAT's 30 s result on these instances moves from run to run, both ways: against the
table of 2026-11-12, this one is better at 25 and 30 requests and leaves stops unserved
at 40 requests on seed 0, where that run had `14 / 3,229.4`. Read a CP-SAT row here as
one draw.

<!-- results:start -->
## Results

**Synthetic Li & Lim-shaped instances** (`--synthetic N`, instance seeds 0 and 1):
customers in a 100 x 100 square, half in six clusters, service time 90, one vehicle per three
requests. Cells are vehicles / distance.

| Requests | Instance seed | engine | CP-SAT |
| ---: | ---: | --- | --- |
| 10 | 0 | **4 / 846.3** | **4 / 846.3** |
| 10 | 1 | **4 / 945.8** | **4 / 945.8** |
| 15 | 0 | **5 / 1,085.2** | **5 / 1,085.2** |
| 15 | 1 | **5 / 1,180** | **5 / 1,180** |
| 20 | 0 | **7 / 1,477.6** | **7 / 1,477.6** |
| 20 | 1 | **6 / 1,512.1** | **6 / 1,512.1** |
| 25 | 0 | **8 / 1,636.5** | 8 / 1,653.3 |
| 25 | 1 | **8 / 1,775.2** | 8 / 1,775.6 |
| 30 | 0 | **8 / 1,996.9** | 8 / 2,011.7 |
| 30 | 1 | **10 / 2,383** | 10 / 2,395.4 |
| 40 | 0 | **11 / 2,776** | incomplete (leaves stops unserved) |
| 40 | 1 | **14 / 2,722.4** | incomplete (leaves stops unserved) |
| 50 | 0 | **13 / 3,369.6** | incomplete (leaves stops unserved) |
| 50 | 1 | **15 / 3,409** | incomplete (leaves stops unserved) |
| 75 | 0 | **18 / 4,569.7** | no solution in time |
| 75 | 1 | **18 / 4,599.6** | no solution in time |
| 100 | 0 | **22 / 5,615.2** | no solution in time |
| 100 | 1 | **24 / 5,755.6** | no solution in time |
| 200 | 0 | **36 / 10,509.9** | no solution in time |
| 200 | 1 | **37 / 11,711.3** | no solution in time |
| 400 | 0 | 51 / 19,982 | not run |

CP-SAT is not run at 400 requests: its model takes minutes and gigabytes to build (at 400 requests it was still being built after 10 minutes at 11 GB, 2026-10-29).

The Li & Lim results are in [`cpsat/PUBLIC_BENCHMARKS.md`](cpsat/PUBLIC_BENCHMARKS.md).

Measured 2026-11-24 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 0. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — a snapshot, not a statistical study.
<!-- results:end -->

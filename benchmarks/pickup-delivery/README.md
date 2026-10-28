# Pickup delivery benchmark

The [`pickup-delivery`](../../use-cases/pickup-delivery) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `pickup_delivery_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in `cpsat/tests`
keep the two cost definitions identical.

```sh
cd benchmarks/pickup-delivery/cpsat
uv sync --all-groups --all-extras      # builds the engine extension too
uv run pytest -q                       # CP-SAT model + parity tests
uv run pickup-delivery-public-bench --help              # Li & Lim PDPTW (Hexaly archive)
uv run pickup-delivery-public-bench --synthetic 200 --seconds 30 --workers 8   # seeded synthetic instance
```

Public-instance protocol and results: [`cpsat/PUBLIC_BENCHMARKS.md`](cpsat/PUBLIC_BENCHMARKS.md).

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded; CP-SAT gets `--workers` search workers and
its time includes model construction.

<!-- results:start -->
## Results

Each plan is re-verified independently: every request served, pickup before dropoff on the
same vehicle, capacity and every time window. Distance is the integral Li & Lim round-trip
metric; lower is better.

**Synthetic Li & Lim-shaped instances** (`--synthetic N`, seed 0): customers in a 100 x 100
square, half in six clusters, service time 90, one vehicle per three requests.

| Requests | engine (vehicles / distance) | CP-SAT (vehicles / distance) |
| --- | --- | --- |
| 50 | **13 / 3,446** | incomplete — drops requests (0hard/-24medium/-3501soft) |
| 100 | **24 / 6,045** | no solution in 30 s |
| 200 | **43 / 11,300** | no solution in 30 s |
| 400 | **83 / 21,617** | not run — the model was still being built after 10 min at 11 GB |

**Li & Lim `lc101`**, first 10 of 53 requests (the public runner's default):

| Solver | Vehicles | Distance | Time |
| --- | --- | --- | --- |
| engine | 2 | **156.000** | 30.0 s |
| CP-SAT | 2 | **156.000** | 3.1 s |

At 10 requests both reach the same plan and CP-SAT proves it first; the exact model stops
scaling by 50–100 requests.

Measured 2026-10-29 on one Apple Silicon Mac (16 cores), 30 s per solver and instance. The
engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction.
Single runs — a snapshot, not a statistical study.
<!-- results:end -->

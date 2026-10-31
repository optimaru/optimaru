# Bin packing benchmark

The [`bin-packing`](../../use-cases/bin-packing) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `bin_packing_cpsat`) on Hexaly BPP template instances, downloaded on demand and
checksum-verified ([`bench/data.py`](bench/data.py)). Both answers are rescored by
the use case's own evaluator (the C++ `constraint_costs`).

```sh
cd benchmarks/bin-packing
uv sync --project cpsat --all-groups --all-extras
uv run --project cpsat pytest -q cpsat/tests
uv run --project cpsat python -m bench.run_bench --seconds 60
# offline: generated instances, the shared engine-vs-CP-SAT runner
uv run --project cpsat python -m bin_packing_cpsat.benchmark --seconds 1 --workers 2 --cpsat-grace 20
```

The CP-SAT plugins (`cpsat/bin_packing_cpsat/constraints.py`) subclass the use case's
constraint specs (`bin_capacity_cost`, `color_conflict_cost`, `item_conflict_cost`,
`used_bins_cost`) and take their default levels and weights from
`bin_packing.constraint_info()`. `cpsat/tests/test_constraint_parity.py` pins every
assignment of small instances in CP-SAT and checks that each constraint's term equals
the engine's raw cost (0 = no violation). CP-SAT accepts only exponent 1 and integer
weights.

<!-- results:start -->
## Results

Hexaly example instances. Lower is better; the best value is bold.

| Instance | Items | Capacity lower bound | engine (0.1.x, pure Python) | CP-SAT |
| --- | --- | --- | --- | --- |
| t60_00 | 60 | 20 | **21** | **21** |
| u120_04 | 120 | 50 | **50** | **50** |
| u250_03 | 250 | 100 | 101 | not run (size cap) |

Bins used. CP-SAT runs only up to the exact-model size cap (`--exact-max-size`). **The engine column
holds 0.1.x numbers** (the pure-Python simulated annealing): the 0.2.0 C++ engine has not been
re-measured on these instances yet, because the Hexaly download was unreachable from the machine
that did the port (on generated instances of the same families it matches or beats the 0.1.x
annealing). Measured 2026-10-29 on one Apple Silicon Mac (16 cores), 30 s per solver and instance. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — a snapshot, not a statistical study.
<!-- results:end -->

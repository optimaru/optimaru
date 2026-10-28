# Bin packing benchmark

The [`bin-packing`](../../use-cases/bin-packing) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `bin_packing_cpsat`) on Hexaly BPP template instances, downloaded on demand and
checksum-verified ([`bench/data.py`](bench/data.py)). Both answers are rescored by
the use case's own evaluator.

```sh
cd benchmarks/bin-packing
uv sync --project cpsat --all-groups --all-extras
uv run --project cpsat pytest -q cpsat/tests
uv run --project cpsat python -m bench.run_bench --seconds 60
```

<!-- results:start -->
## Results

Hexaly example instances. Lower is better; the best value is bold.

| Instance | Items | Capacity lower bound | engine | CP-SAT |
| --- | --- | --- | --- | --- |
| t60_00 | 60 | 20 | **21** | **21** |
| u120_04 | 120 | 50 | **50** | **50** |
| u250_03 | 250 | 100 | 101 | not run (size cap) |

Bins used. CP-SAT runs only up to the exact-model size cap (`--exact-max-size`). Measured 2026-10-29 on one Apple Silicon Mac (16 cores), 30 s per solver and instance. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — a snapshot, not a statistical study.
<!-- results:end -->

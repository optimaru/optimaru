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


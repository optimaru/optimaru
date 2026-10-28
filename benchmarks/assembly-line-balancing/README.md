# Assembly line balancing benchmark

The [`assembly-line-balancing`](../../use-cases/assembly-line-balancing) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `assembly_line_balancing_cpsat`) on SALBP-1 instances (Hexaly archive), downloaded on demand and
checksum-verified ([`bench/data.py`](bench/data.py)). Both answers are rescored by
the use case's own evaluator.

```sh
cd benchmarks/assembly-line-balancing
uv sync --project cpsat --all-groups --all-extras
uv run --project cpsat pytest -q cpsat/tests
uv run --project cpsat python -m bench.run_bench --seconds 60
```

Recorded results: [`bench/RESULTS.md`](bench/RESULTS.md).

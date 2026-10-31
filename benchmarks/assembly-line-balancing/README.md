# Assembly line balancing benchmark

The [`assembly-line-balancing`](../../use-cases/assembly-line-balancing) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `assembly_line_balancing_cpsat`) on SALBP-1 instances (the Otto et al. SALBP archive from assembly-line-balancing.de, as used by Hexaly), downloaded on demand and
checksum-verified ([`bench/data.py`](bench/data.py)). Both answers are rescored by
the use case's own evaluator.

```sh
cd benchmarks/assembly-line-balancing
uv sync --project cpsat --all-groups --all-extras
uv run --project cpsat pytest -q cpsat/tests
uv run --project cpsat python -m bench.run_bench --seconds 60
```

Recorded results: [`bench/RESULTS.md`](bench/RESULTS.md).

<!-- results:start -->
## Results

SALBP large data set (n=100). Lower is better; the best value is bold.

| Instance | Tasks | Best known | engine | CP-SAT |
| --- | --- | --- | --- | --- |
| instance_n=100_1 | 100 | 23 | 26 | **23** |
| instance_n=100_10 | 100 | 22 | 25 | **22** |
| instance_n=100_100 | 100 | 25 | 29 | **25** |

Stations used. Measured 2026-10-29 on one Apple Silicon Mac (16 cores), 30 s per solver and instance. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — a snapshot, not a statistical study.
<!-- results:end -->

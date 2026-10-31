# Assembly line balancing benchmark

The [`assembly-line-balancing`](../../use-cases/assembly-line-balancing) use case (the C++
engine) against **OR-Tools CP-SAT** ([`cpsat/`](cpsat), package `assembly_line_balancing_cpsat`)
on SALBP-1 instances (the Otto et al. SALBP archive from assembly-line-balancing.de, as used by
Hexaly), downloaded on demand and checksum-verified ([`bench/data.py`](bench/data.py)). Both
answers are rescored by the use case's own evaluator (`constraint_costs()`).

The CP-SAT model is built from [`cpsat-common`](../cpsat-common) plugins, one per engine
constraint (`assembly_line_balancing_cpsat/constraints.py`, subclassing the domain specs; the
defaults come from the engine's `constraint_info()`). Every encoding is exact, so the same
assignment gives the same raw cost per constraint in both solvers
(`cpsat/tests/test_constraint_parity.py` pins every assignment of small instances). Not
supported on CP-SAT: `exponent != 1` and non-integer weights (refused).

```sh
cd benchmarks/assembly-line-balancing/cpsat
uv sync --all-groups --all-extras
uv run pytest -q tests
uv run python -m assembly_line_balancing_cpsat.benchmark --seconds 10     # generated lines, no download
cd .. && uv run --project cpsat python -m bench.run_bench --seconds 60   # the SALBP archive
```

Recorded results: [`bench/RESULTS.md`](bench/RESULTS.md).

<!-- results:start -->
## Results

SALBP large data set (n=100). Lower is better; the best value is bold.

| Instance | Tasks | Best known | engine (0.1.5, pure Python) | CP-SAT |
| --- | --- | --- | --- | --- |
| instance_n=100_1 | 100 | 23 | 26 | **23** |
| instance_n=100_10 | 100 | 22 | 25 | **22** |
| instance_n=100_100 | 100 | 25 | 29 | **25** |

Stations used. The engine column is the 0.1.5 pure-Python simulated annealing; the 0.2.0 C++
engine (CloseStationMove, the precedence-window recommender) has not been re-measured on the
archive yet. Measured 2026-10-29 on one Apple Silicon Mac (16 cores), 30 s per solver and instance. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — a snapshot, not a statistical study.
<!-- results:end -->

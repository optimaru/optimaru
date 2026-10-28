# Quadratic assignment benchmark

The [`quadratic-assignment`](../../use-cases/quadratic-assignment) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `quadratic_assignment_cpsat`) on QAPLIB (Hexaly archive) with QAPLIB best-known solutions, downloaded on demand and
checksum-verified ([`bench/data.py`](bench/data.py)). Both answers are rescored by
the use case's own evaluator.

```sh
cd benchmarks/quadratic-assignment
uv sync --project cpsat --all-groups --all-extras
uv run --project cpsat pytest -q cpsat/tests
uv run --project cpsat python -m bench.run_bench --seconds 60
```

Recorded results: [`bench/RESULTS.md`](bench/RESULTS.md).

<!-- results:start -->
## Results

Hexaly example instances. Lower is better; the best value is bold.

| Instance | Size | QAPLIB best known | engine | CP-SAT |
| --- | --- | --- | --- | --- |
| nug30 | 30 | 6124 | 6,156 | not run (size cap) |
| lipa30a | 30 | 13178 | 13,395 | not run (size cap) |
| tai30a | 30 | 1818146 | 1,864,536 | not run (size cap) |

Objective value. CP-SAT runs only up to the exact-model size cap (`--exact-max-size`); these n=30 instances are above it. Measured 2026-10-29 on one Apple Silicon Mac (16 cores), 30 s per solver and instance. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — a snapshot, not a statistical study.
<!-- results:end -->

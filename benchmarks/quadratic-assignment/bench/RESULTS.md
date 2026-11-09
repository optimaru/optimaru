# QAPLIB benchmark snapshot

Measured on 2026-11-04 with the C++ engine on an Apple M4 Max (16 logical CPUs), one engine
process, seed 1, 30 s per instance. The solve budget excludes archive download and parsing.
The solution is re-scored by `quadratic_assignment.constraint_costs`.

| instance | facilities | solver | budget | objective | QAPLIB BKS | gap |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| `nug30` | 30 | engine | 30 s | 6,256 | 6,124 | 2.16% |
| `lipa30a` | 30 | engine | 30 s | 13,403 | 13,178 | 1.71% |
| `tai30a` | 30 | engine | 30 s | 1,883,012 | 1,818,146 | 3.57% |

These three instances are a small sample, not an aggregate result. Running
every instance of the benchmark's QAPLIB selection (33 instances with 30 to 50
facilities) is explicit:

```sh
cd benchmarks/quadratic-assignment && uv run --project cpsat python -m bench.run_bench --all --seconds 60 --json
```

The CP-SAT reference is disabled by default on these instances because its
current linearization creates O(n^4) pair variables. Use `--exact-max-size N`
when measuring that model deliberately; build time is included in its result.

# QAPLIB benchmark snapshot

Measured on 2026-08-17 with an Apple M4 Pro (14 logical CPUs, 48 GiB), macOS
26.3.1, one engine process, seed 1. The solve budget excludes archive download
and parsing. The solution is re-scored by `quadratic_assignment.constraint_costs`.

| instance | tasks | solver | budget | objective | QAPLIB BKS | gap |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| `tai30a` | 30 | engine | 60 s | 1,859,862 | 1,818,146 | 2.29% |
| `nug30` | 30 | engine | 60 s | 6,156 | 6,124 | 0.52% |

Hexaly reports a 1.1% average 60-second gap over 26 Taillard instances. Only
`tai30a` above belongs to that family, so this table must not be read as an
aggregate head-to-head result. Running all available archive instances is
explicit:

```sh
cd benchmarks/quadratic-assignment && uv run --project cpsat python -m bench.run_bench --all --seconds 60 --json
```

The CP-SAT reference is disabled by default on these instances because its
current linearization creates O(n^4) pair variables. Use `--exact-max-size N`
when measuring that model deliberately; build time is included in its result.

# SALBP benchmark snapshot

Measured on 2026-11-04 with the C++ engine on an Apple M4 Max (16 logical CPUs), one
engine process, seed 1, 30 s per solver and instance; CP-SAT uses 8 workers. The solve
budget excludes archive download and parsing. Every solution is independently checked for
capacity and precedence feasibility before its station count is reported.

| instance | tasks | solver | budget | stations | official BKS | gap |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| `instance_n=100_1` | 100 | engine | 30 s | 24 | 23 | 4.35% |
| `instance_n=100_1` | 100 | CP-SAT | 2.2 s | 23 | 23 | 0.00% |
| `instance_n=100_10` | 100 | engine | 30 s | 22 | 22 | 0.00% |
| `instance_n=100_10` | 100 | CP-SAT | 1.8 s | 22 | 22 | 0.00% |
| `instance_n=100_100` | 100 | engine | 30 s | 26 | 25 | 4.00% |
| `instance_n=100_100` | 100 | CP-SAT | 6.2 s | 25 | 25 | 0.00% |

CP-SAT's budget column is the time it took to prove the optimum. Hexaly reports a 0.2%
average 60-second gap over the complete 525-instance large and 525-instance very-large
collections; the three rows above are a sample, not an equivalent aggregate. The complete
sweep is available through `--all`.

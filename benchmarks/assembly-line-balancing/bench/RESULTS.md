# SALBP benchmark snapshot

> **0.1.5 numbers.** The engine rows were measured with the 0.1.5 pure-Python
> simulated-annealing engine. The 0.2.0 C++ engine (CloseStationMove, the precedence-window
> value recommender, late acceptance with ruin & recreate) has not been re-measured on the
> archive yet; re-run `python -m bench.run_bench` and replace this snapshot.

Measured on 2026-08-17 with an Apple M4 Pro (14 logical CPUs, 48 GiB), macOS
26.3.1, one engine process, seed 1. The solve budget excludes archive download
and parsing. Every solution is independently checked for capacity and
precedence feasibility before its station count is reported.

| instance | tasks | solver | budget | stations | official BKS | gap |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| `instance_n=100_1` | 100 | engine | 60 s | 26 | 23 | 13.04% |
| `instance_n=20_1` | 20 | engine | 2 s | 3 | 3 | 0.00% |
| `instance_n=20_1` | 20 | CP-SAT | 2 s | 3 | 3 | 0.00% |

Hexaly reports a 0.2% average 60-second gap over the complete 525-instance
large and 525-instance very-large collections. The single large row above is
therefore diagnostic, not an equivalent aggregate. It exposed that the 0.1.5
single-task neighborhood is weak on large SALBP even though it remains
feasible; the 0.2.0 CloseStationMove targets that gap. The complete sweep is
available through `--all`.

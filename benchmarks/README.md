# Benchmarks

Public, reproducible comparisons of the engine's use cases against published
instance sets and other solvers. Instance archives are downloaded on demand,
checksum-verified and kept out of Git. Every returned solution is rescored
independently by the harness; a solver's own reported objective is never
taken as the result by itself.

| Directory | Problem | Public data | Compared against |
| --- | --- | --- | --- |
| [`vehicle-routing`](vehicle-routing) | CVRP | CVRPLIB Set X, Augerat (Hexaly bundle) | best-known solutions, PyVRP, OR-Tools CP-SAT |

Each benchmark is its own `uv` project that depends on the matching
`use-cases/<name>` packages by path, so a run always measures the engine in
this checkout.

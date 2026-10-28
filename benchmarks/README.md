# Benchmarks

Reproducible comparisons of the engine's use cases against published
instance sets and other open-source solvers. The competing solvers live only
here, never in `use-cases/`. Instance data is downloaded on demand and kept
out of Git, and every returned solution is rescored independently by the
harness; a solver's own reported objective is never taken as the result.

| Directory | Problem | Public data | Compared against |
| --- | --- | --- | --- |
| [`vehicle-routing`](vehicle-routing) | CVRP | CVRPLIB Set X, Augerat | best-known solutions, OR-Tools Routing, Timefold Solver |

Each benchmark is its own `uv` project that depends on the matching
`use-cases/<name>` package by path, so a run always measures the engine in
this checkout.

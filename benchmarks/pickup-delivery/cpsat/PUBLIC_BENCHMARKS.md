# PDPTW public comparison

Source: the 100-customer Li & Lim set from SINTEF's TOP benchmark page
(`https://www.sintef.no/projectweb/top/pdptw/li-lim-benchmark/`, archive
`pdp_100.zip`, SHA-256 pinned). The local contracts use integer
travel time, so continuous Euclidean edge lengths are projected to the nearest
integer for both backends. The result is therefore a backend comparison on the
same official coordinates and constraints, not a bit-identical reproduction
of the published two-decimal objective.

Both local backends use the same comparison objective: assign every request,
then minimize integral travel distance. Vehicle count is reported as a route
diagnostic, but is not encoded as a giant fixed cost: that scalarization creates
an artificial objective cliff for a local-search solver. Reproducing the
benchmark's fleet-count-first objective requires a real lexicographic or two-stage solve.

Fast comparison on the first ten request pairs:

```bash
uv run --project benchmarks/pickup-delivery/cpsat \
  python -m pickup_delivery_cpsat.public_benchmark \
  --instance lc101 --requests 10 --seconds 10 --backend both
```

Use `--requests 0` for every pair in the source file. The CP-SAT circuit model
grows quadratically per vehicle, so the full run needs a materially larger
budget.

Observed locally on 2026-08-17 with one worker and seed 0:

| Backend | Vehicles | Integral distance |
|---|---:|---:|
| Engine | 2 | 156 |
| CP-SAT | 2 | 156 |

The independent scorer verifies every stop exactly once, same vehicle,
pickup-before-delivery, running load, all time windows and depot return time.

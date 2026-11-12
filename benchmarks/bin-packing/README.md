# Bin packing benchmark

The [`bin-packing`](../../docs/use-cases/bin-packing) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `bin_packing_cpsat`) on Falkenauer's uniform (u) and triplet (t)
instances from OR-Library, downloaded on demand and checksum-verified ([`public_benchmark_data.py`](cpsat/bin_packing_cpsat/public_benchmark_data.py)). Both answers are rescored by
the use case's own evaluator (the C++ `constraint_costs`).

```sh
cd benchmarks/bin-packing/cpsat
OPTIMARU_USE_CASES=bin_packing uv sync   # also builds this checkout's optimaru (bin_packing only)
uv run pytest -q
uv run bin-packing-public-bench --seconds 60
uv run bin-packing-public-bench --sample-per-class 3 --seconds 30   # instances 00, 10 and 19 of each class
# offline: generated instances, the shared engine-vs-CP-SAT runner
uv run python -m bin_packing_cpsat.benchmark --seconds 1 --workers 2 --cpsat-grace 20
# offline: search quality against a recorded baseline on generated coloured instances (exit 1 = regression)
uv run bin-packing-quality-bench --seconds 2 --seeds 1,2,3 --check
```

The CP-SAT plugins ([`external_solver.bin_packing.constraints`](https://github.com/optimaru/external-solver/blob/v0.3.0/src/external_solver/bin_packing/constraints.py)) subclass the use case's
constraint specs (`bin_capacity_cost`, `color_conflict_cost`, `item_conflict_cost`,
`used_bins_cost`) and take their default levels and weights from
`bin_packing.constraint_info()`. External-solver's `tests/bin_packing/test_constraint_parity.py` pins every
assignment of small instances in CP-SAT and checks that each constraint's term equals
the engine's raw cost (0 = no violation). `level="soft"` / `level="hard"` moves a term to
the soft / hard level in both solvers, and `level="medium"` is refused by both (`this use
case has no medium level`). CP-SAT accepts only exponent 1 and integer weights.

A constraint may be picked several times (as specs or as
`{"name", "weight", "level", "offset", "clip"}` mappings); each pick builds its own terms with
its own knobs, its level included. The three per-bin constraints add one term per bin
0..n-1, an empty bin included, and `used_bins_cost` one term for the bin count, so an
integral `offset` shapes each bin (or the count) exactly as the engine does in both `clip`
modes; a fractional offset is refused (`offset must be an integer for CP-SAT`).
`clip=False` with an offset on a hard pick is refused with the engine's message when the
objective is built.

<!-- results:start -->
## Results

Falkenauer: instances 00, 10 and 19 of each class. Bins used; lower is better and the best value is bold.

| Instance | Items | Best known | optimaru engine | CP-SAT |
| --- | ---: | ---: | ---: | ---: |
| u120_00 | 120 | 48 | **48** | **48** |
| u120_10 | 120 | 52 | **52** | 53 |
| u120_19 | 120 | 50 | **49** | 50 |
| u250_00 | 250 | 99 | 99 | not run (size cap) |
| u250_10 | 250 | 105 | 105 | not run (size cap) |
| u250_19 | 250 | 102 | 102 | not run (size cap) |
| u500_00 | 500 | 198 | 198 | not run (size cap) |
| u500_10 | 500 | 200 | 200 | not run (size cap) |
| u500_19 | 500 | 196 | 196 | not run (size cap) |
| u1000_00 | 1000 | 399 | 399 | not run (size cap) |
| u1000_10 | 1000 | 400 | 400 | not run (size cap) |
| u1000_19 | 1000 | 400 | 400 | not run (size cap) |
| t60_00 | 60 | 20 | **20** | **20** |
| t60_10 | 60 | 20 | **20** | **20** |
| t60_19 | 60 | 20 | **20** | **20** |
| t120_00 | 120 | 40 | **40** | **40** |
| t120_10 | 120 | 40 | **40** | **40** |
| t120_19 | 120 | 40 | **40** | **40** |
| t249_00 | 249 | 83 | 83 | not run (size cap) |
| t249_10 | 249 | 83 | 83 | not run (size cap) |
| t249_19 | 249 | 83 | 83 | not run (size cap) |
| t501_00 | 501 | 167 | 167 | not run (size cap) |
| t501_10 | 501 | 167 | 167 | not run (size cap) |
| t501_19 | 501 | 167 | 167 | not run (size cap) |

CP-SAT runs only up to the exact-model size cap (`--exact-max-size`, 120 items). Measured 2026-11-12 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — a snapshot, not a statistical study.
<!-- results:end -->

"Best known" is the bin count the OR-Library file states. For u120_19 the file states 50;
49 is the capacity bound, ceil(total weight / capacity), and a packing with 49 bins exists.

## Search quality against a recorded baseline

A search review found the C++ search 1-2 bins behind a pure-Python simulated-annealing
baseline on instances with colours and conflicts: its only bin-emptying move, `EmptyBinMove`,
checks the capacity only, so emptying a bin broke a colour or a conflict and late acceptance
turned the move down. `RepackMove` closed that gap; `TradeMove` and the fill tie-break, which
came later, closed the one left on the plain instances (see the use case's
[Search](../../docs/use-cases/bin-packing/README.md#search)).
`bin-packing-quality-bench` ([`quality_bench.py`](cpsat/bin_packing_cpsat/quality_bench.py)) generates every instance below
(`--extra` for those after the first four pairs); `--check` fails when a run on the
review's four coloured instances uses more bins than the recorded baseline (the most bins
the annealing used over seeds 1-3: sq1 44, g11 43, g22 45, g33 43), or any run ends
infeasible.

Bins used, seeds 1, 2, 3, one thread, measured 2026-11-12; no run ended infeasible. "LB" is
the weight lower bound, ceil(total weight / capacity). The columns are the engine with parts
of its search switched off in `SolverConfig`: `repack_moves`, `trade_moves` and
`fill_tie_break` all three, `repack_moves` alone, and `trade_moves` with `fill_tie_break`
(the search of 0.5.0). The last three columns are the runs of 2 s; those of 10 s gave the
same number in every cell.

| instance (items, LB) | all three off 2 s | all three off 10 s | `repack_moves` off | `trade_moves` and `fill_tie_break` off | engine |
| --- | --- | --- | --- | --- | --- |
| sq1 (120, 43) | 45, 45, 45 | 45, 45, 45 | 43, 43, 43 | 43, 43, 43 | 43, 43, 43 |
| g11 (120, 42) | 45, 45, 45 | 44, 45, 45 | 42, 42, 42 | 42, 42, 42 | 42, 42, 42 |
| g22 (120, 44) | 45, 45, 46 | 45, 45, 46 | 44, 44, 44 | 44, 44, 44 | 44, 44, 44 |
| g33 (120, 42) | 44, 44, 43 | 44, 44, 43 | 42, 42, 42 | 42, 42, 42 | 42, 42, 42 |
| colours-only-sq1 (120, 43) | 45, 45, 45 | 45, 45, 45 | 43, 43, 43 | 43, 43, 43 | 43, 43, 43 |
| review-250 (250, 86) | 88, 89, 89 | 88, 89, 89 | 86, 86, 86 | 86, 86, 86 | 86, 86, 86 |
| plain-sq1 (120, 43) | 44, 44, 44 | 44, 44, 44 | 43, 43, 43 | 43, 43, 43 | 43, 43, 43 |
| plain-g11 (120, 42) | 43, 43, 43 | 43, 43, 43 | 42, 42, 42 | 42, 42, 42 | 42, 42, 42 |
| plain-g22 (120, 44) | 44, 44, 44 | 44, 44, 44 | 44, 44, 44 | 44, 44, 44 | 44, 44, 44 |
| plain-g33 (120, 42) | 42, 42, 42 | 42, 42, 42 | 42, 42, 42 | 42, 42, 42 | 42, 42, 42 |
| web-demo (20, 8) | 8, 8, 8 | 8, 8, 8 | 8, 8, 8 | 8, 8, 8 | 8, 8, 8 |
| web-demo-x6 (120, 45) | 48, 48, 48 | 48, 48, 48 | 47, 47, 47 | 47, 48, 48 | 47, 47, 47 |
| bench-small (12, 5) | 6, 6, 6 | 6, 6, 6 | 6, 6, 6 | 6, 6, 6 | 6, 6, 6 |
| bench-medium (30, 12) | 12, 12, 12 | 12, 12, 12 | 12, 12, 12 | 12, 12, 12 | 12, 12, 12 |
| bench-large (60, 26) | 26, 26, 26 | 26, 26, 26 | 26, 26, 26 | 26, 26, 26 | 26, 26, 26 |
| u500-coloured (500, 198) | 207, 206, 208 | 207, 206, 207 | 198, 198, 198 | 199, 199, 200 | 198, 198, 198 |
| u250 (250, 106) | 107, 107, 107 | 107, 107, 107 | 106, 106, 106 | 107, 107, 107 | 106, 106, 106 |
| u500 (500, 199) | 201, 201, 201 | 201, 201, 201 | 199, 199, 199 | 200, 200, 200 | 199, 199, 199 |
| u1000 (1000, 399) | 403, 403, 403 | 403, 403, 403 | 399, 399, 399 | 401, 402, 401 | 399, 399, 399 |
| t120 (120, 40) | 43, 43, 43 | 42, 42, 43 | 41, 41, 41 | 41, 41, 41 | 40, 40, 40 |

- sq1, g11, g22, g33: the review's recipe (120 items, weights 10..60, capacity 100,
  about 40% of the items in one of 4 colours, 10 conflict pairs; generator seeds
  20261031, 11, 22, 33). `plain-*`: the same weights without colours or conflicts.
  `colours-only-sq1`: sq1 without its conflicts. `review-250`: the same recipe with
  250 items and 25 conflicts.
- web-demo: the web sample (20 items, colours, 3 conflicts); web-demo-x6: six copies of
  it (120 items, 18 conflicts).
- bench-small / -medium / -large: the instances of `bin_packing_cpsat.benchmark`.
  u250 / u500 / u1000: the same uniform recipe (weights 20..100, capacity 150) at
  those sizes; u500-coloured adds its colours (every third item, 4 colours) and 50
  conflicts. t120: 40 triplets that each fill a bin of 1000 exactly (40 bins optimal).

Measured on one Apple M4 Max Mac (16 cores), eight solves at a time. Single runs per
seed — a snapshot, not a statistical study.

# Bin packing benchmark

The [`bin-packing`](../../use-cases/bin-packing) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `bin_packing_cpsat`) on Hexaly BPP template instances, downloaded on demand and
checksum-verified ([`bench/data.py`](bench/data.py)). Both answers are rescored by
the use case's own evaluator (the C++ `constraint_costs`).

```sh
cd benchmarks/bin-packing
uv sync --project cpsat --all-groups --all-extras
uv run --project cpsat pytest -q cpsat/tests
uv run --project cpsat python -m bench.run_bench --seconds 60
# offline: generated instances, the shared engine-vs-CP-SAT runner
uv run --project cpsat python -m bin_packing_cpsat.benchmark --seconds 1 --workers 2 --cpsat-grace 20
# offline: search quality against 0.1.5 on generated coloured instances (exit 1 = regression)
uv run --project cpsat python -m bench.quality --seconds 2 --seeds 1,2,3 --check
```

The CP-SAT plugins (`cpsat/bin_packing_cpsat/constraints.py`) subclass the use case's
constraint specs (`bin_capacity_cost`, `color_conflict_cost`, `item_conflict_cost`,
`used_bins_cost`) and take their default levels and weights from
`bin_packing.constraint_info()`. `cpsat/tests/test_constraint_parity.py` pins every
assignment of small instances in CP-SAT and checks that each constraint's term equals
the engine's raw cost (0 = no violation). CP-SAT accepts only exponent 1 and integer
weights.

<!-- results:start -->
## Results

Hexaly example instances. Lower is better; the best value is bold.

| Instance | Items | Capacity lower bound | engine (0.1.x, pure Python) | CP-SAT |
| --- | --- | --- | --- | --- |
| t60_00 | 60 | 20 | **21** | **21** |
| u120_04 | 120 | 50 | **50** | **50** |
| u250_03 | 250 | 100 | 101 | not run (size cap) |

Bins used. CP-SAT runs only up to the exact-model size cap (`--exact-max-size`). **The engine column
holds 0.1.x numbers** (the pure-Python simulated annealing): the 0.2.0 C++ engine has not been
re-measured on these instances yet, because the Hexaly download was unreachable from the machine
that did the port (on generated instances of the same families it matches or beats the 0.1.x
annealing, see [below](#search-quality-against-015)). Measured 2026-10-29 on one Apple Silicon Mac (16 cores), 30 s per solver and instance. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — a snapshot, not a statistical study.
<!-- results:end -->

## Search quality against 0.1.5

The 0.2.0 review found the C++ search 1-2 bins behind 0.1.5's pure-Python annealing on
instances with colours and conflicts: its only bin-emptying move, `EmptyBinMove`, checks
the capacity only, so emptying a bin broke a colour or a conflict and late acceptance
turned the move down. `RepackMove` (see the use case's
[Search](../../use-cases/bin-packing/README.md#search)) closes that gap.
[`bench/quality.py`](bench/quality.py) generates every instance below
(`--extra` for those after the first four pairs); `--check` fails when a run on the
review's four coloured instances uses more bins than 0.1.5 did, or any run ends
infeasible.

Bins used, seeds 1, 2, 3, one thread; \* = ended infeasible. "LB" is the weight lower
bound, ceil(total weight / capacity). 0.1.5 is optimaru 0.1.5 from PyPI; it stops by
itself after its 50,000 iterations (before 10 s on all but the 500- and 1000-item
instances), so its 10 s runs mostly repeat the 2 s ones. "0.2.0 without RepackMove" is
the release candidate the review measured (commit b9d70f0).

| instance (items, LB) | 0.1.5 2 s | 0.1.5 10 s | 0.2.0 without RepackMove 2 s | 0.2.0 without RepackMove 10 s | 0.2.0 2 s | 0.2.0 10 s |
| --- | --- | --- | --- | --- | --- | --- |
| sq1 (120, 43) | 44, 44\*, 44 | 44, 44, 44 | 46, 45, 46 | 45, 45, 46 | 43, 43, 43 | 43, 43, 43 |
| g11 (120, 42) | 43, 43, 43 | 43, 43, 43 | 45, 44, 44 | 45, 44, 44 | 42, 42, 42 | 42, 42, 42 |
| g22 (120, 44) | 45, 45, 44 | 45, 45, 44 | 46, 45, 46 | 46, 45, 46 | 44, 44, 44 | 44, 44, 44 |
| g33 (120, 42) | 43, 43, 43 | 43, 43, 43 | 44, 44, 43 | 44, 44, 43 | 42, 42, 42 | 42, 42, 42 |
| colours-only-sq1 (120, 43) | 44, 45, 45 | 44, 44, 44 | 46, 46, 46 | 46, 45, 46 | 43, 43, 43 | 43, 43, 43 |
| review-250 (250, 86) | 87\*, 88\*, 87\* | 88\*, 88\*, 87\* | 88, 91, 91 | 88, 89, 89 | 86, 86, 86 | 86, 86, 86 |
| plain-sq1 (120, 43) | 44, 44, 44 | 44, 44, 44 | 44, 44, 44 | 44, 44, 44 | 43, 43, 43 | 43, 43, 43 |
| plain-g11 (120, 42) | 43, 43, 43 | 43, 43, 43 | 43, 43, 43 | 43, 43, 43 | 42, 42, 42 | 42, 42, 42 |
| plain-g22 (120, 44) | 44, 44, 44 | 44, 44, 44 | 44, 44, 44 | 44, 44, 44 | 44, 44, 44 | 44, 44, 44 |
| plain-g33 (120, 42) | 42, 42, 42 | 42, 42, 42 | 42, 42, 42 | 42, 42, 42 | 42, 42, 42 | 42, 42, 42 |
| web-demo (20, 8) | 8, 8, 8 | 8, 8, 8 | 8, 8, 8 | 8, 8, 8 | 8, 8, 8 | 8, 8, 8 |
| web-demo-x6 (120, 45) | 48, 49, 48\* | 48, 49, 48\* | 49, 48, 48 | 48, 48, 48 | 47, 48, 47 | 47, 47, 47 |
| bench-small (12, 5) | 6, 6, 6 | 6, 6, 6 | 6, 6, 6 | 6, 6, 6 | 6, 6, 6 | 6, 6, 6 |
| bench-medium (30, 12) | 12, 12, 12 | 12, 12, 12 | 12, 12, 12 | 12, 12, 12 | 12, 12, 12 | 12, 12, 12 |
| bench-large (60, 26) | 26, 26, 26 | 26, 26, 26 | 26, 26, 26 | 26, 26, 26 | 26, 26, 26 | 26, 26, 26 |
| u500-coloured (500, 198) | 201\*, 202\*, 202\* | 201\*, 202\*, 202\* | 211, 210, 210 | 211, 210, 207 | 199, 199, 199 | 199, 199, 199 |
| u250 (250, 106) | 107, 107, 107 | 107, 107, 107 | 107, 107, 107 | 107, 107, 107 | 106, 107, 106 | 106, 107, 106 |
| u500 (500, 199) | 201, 201, 201 | 201, 201, 201 | 201, 201, 201 | 201, 201, 201 | 200, 200, 200 | 200, 200, 200 |
| u1000 (1000, 399) | 403, 403, 403 | 403, 403, 403 | 403, 403, 403 | 403, 403, 403 | 401, 401, 401 | 401, 401, 401 |
| t120 (120, 40) | 44, 44, 44 | 44, 44, 44 | 43, 43, 43 | 42, 42, 43 | 41, 41, 41 | 41, 41, 41 |

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

Measured 2026-10-31 on a shared 4-core Linux VM (other jobs running, two solves at a
time). Single runs per seed — a snapshot, not a statistical study.

# QAPLIB benchmark snapshot

One engine process per instance. The solve budget excludes archive download
and parsing, and the solution is re-scored by
`quadratic_assignment.constraint_costs`. The best-known value of an instance is
what its QAPLIB reference permutation scores under the same evaluator, read in
whichever index convention the solution file uses (facility per location, or
location per facility). That is the value the file states, except for `kra32`,
whose file states 88900 (the value of `kra30a`) while its permutation scores
88700: `REFERENCE_ERRATA` in `bench/run_bench.py` records it, and any other
mismatch stops the run.

<!-- results:start -->
The 33 QAPLIB instances with 30 to 50 facilities. Lower is better; the gap is to the
QAPLIB best-known value. CP-SAT runs only up to the exact-model size cap (`--exact-max-size`);
these instances are above it.

The archive has no solution file for esc32a, esc32b, esc32c, esc32d, esc32h, so they have no reference and no gap.

| Instance | Size | QAPLIB best known | optimaru engine | Gap |
| --- | ---: | ---: | ---: | ---: |
| esc32a | 32 | — | 130 | — |
| esc32b | 32 | — | 168 | — |
| esc32c | 32 | — | 642 | — |
| esc32d | 32 | — | 200 | — |
| esc32e | 32 | 2 | 2 | 0.00% |
| esc32g | 32 | 6 | 6 | 0.00% |
| esc32h | 32 | — | 438 | — |
| kra30a | 30 | 88,900 | 88,900 | 0.00% |
| kra30b | 30 | 91,420 | 91,420 | 0.00% |
| kra32 | 32 | 88,700 | 88,700 | 0.00% |
| lipa30a | 30 | 13,178 | 13,178 | 0.00% |
| lipa30b | 30 | 151,426 | 151,426 | 0.00% |
| lipa40a | 40 | 31,538 | 31,538 | 0.00% |
| lipa40b | 40 | 476,581 | 476,581 | 0.00% |
| lipa50a | 50 | 62,093 | 62,093 | 0.00% |
| lipa50b | 50 | 1,210,244 | 1,210,244 | 0.00% |
| nug30 | 30 | 6,124 | 6,124 | 0.00% |
| sko42 | 42 | 15,812 | 15,816 | 0.03% |
| sko49 | 49 | 23,386 | 23,412 | 0.11% |
| ste36a | 36 | 9,526 | 9,526 | 0.00% |
| ste36b | 36 | 15,852 | 15,852 | 0.00% |
| ste36c | 36 | 8,239,110 | 8,239,110 | 0.00% |
| tai30a | 30 | 1,818,146 | 1,831,494 | 0.73% |
| tai30b | 30 | 637,117,113 | 637,117,113 | 0.00% |
| tai35a | 35 | 2,422,002 | 2,458,204 | 1.49% |
| tai35b | 35 | 283,315,445 | 283,360,944 | 0.02% |
| tai40a | 40 | 3,139,370 | 3,186,052 | 1.49% |
| tai40b | 40 | 637,250,948 | 637,250,948 | 0.00% |
| tai50a | 50 | 4,938,796 | 5,042,698 | 2.10% |
| tai50b | 50 | 458,821,517 | 458,993,732 | 0.04% |
| tho30 | 30 | 149,936 | 149,936 | 0.00% |
| tho40 | 40 | 240,516 | 240,712 | 0.08% |
| wil50 | 50 | 48,816 | 48,878 | 0.13% |

Mean gap 0.22%, median 0.00%, worst 2.10%. Measured 2026-11-11 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. Single runs — a snapshot, not a statistical study.
<!-- results:end -->

The table is the benchmark's whole QAPLIB selection (every instance with 30 to
50 facilities):

```sh
cd benchmarks/quadratic-assignment && uv run --project cpsat python -m bench.run_bench --all --seconds 30 --json
```

The CP-SAT reference is disabled by default on these instances because its
current linearization creates O(n^4) pair variables. Use `--exact-max-size N`
when measuring that model deliberately; build time is included in its result.

# SALBP benchmark snapshot

One engine process per instance; CP-SAT uses 8 workers. The solve budget
excludes archive download and parsing. Every solution is rescored by the use
case's evaluator (`constraint_costs()`), which must report zero hard cost
(capacity, precedence and the other hard constraints) before its station count is
reported. The reference is the upper bound in the archive's own details
workbook (`best_known` in the JSON): the optimum where the workbook's lower
bound meets it, and otherwise the best solution its authors' solver found in
100 seconds, so on the 1,000-task instances a result below it is possible.

<!-- results:start -->
SALBP-1: instances 1, 125, 250, 375 and 500 of each data set. Stations used; lower is better and the best value is bold. The upper bound
is the one in the archive's details workbook: the optimum where the workbook's lower bound meets
it, otherwise the best its authors' solver found in 100 s.

| Data set | Instance | Tasks | Archive upper bound | optimaru engine | CP-SAT |
| --- | --- | ---: | ---: | ---: | ---: |
| small | instance_n=20_1 | 20 | 3 | **3** | **3** |
| small | instance_n=20_125 | 20 | 5 | **5** | **5** |
| small | instance_n=20_250 | 20 | 10 | **10** | **10** |
| small | instance_n=20_375 | 20 | 3 | **3** | **3** |
| small | instance_n=20_500 | 20 | 8 | **8** | **8** |
| medium | instance_n=50_1 | 50 | 8 | **8** | **8** |
| medium | instance_n=50_125 | 50 | 33 | **33** | **33** |
| medium | instance_n=50_250 | 50 | 7 | **7** | **7** |
| medium | instance_n=50_375 | 50 | 13 | **13** | **13** |
| medium | instance_n=50_500 | 50 | 34 | **34** | **34** |
| large | instance_n=100_1 | 100 | 23 | 24 | **23** |
| large | instance_n=100_125 | 100 | 14 | **14** | **14** |
| large | instance_n=100_250 | 100 | 24 | **24** | **24** |
| large | instance_n=100_375 | 100 | 57 | 59 | **57** |
| large | instance_n=100_500 | 100 | 14 | **14** | **14** |
| very-large | instance_n=1000_1 | 1000 | 135 | 137 | not run (size cap) |
| very-large | instance_n=1000_125 | 1000 | 546 | 530 | not run (size cap) |
| very-large | instance_n=1000_250 | 1000 | 140 | 153 | not run (size cap) |
| very-large | instance_n=1000_375 | 1000 | 229 | 233 | not run (size cap) |
| very-large | instance_n=1000_500 | 1000 | 591 | 555 | not run (size cap) |

CP-SAT runs only up to the exact-model size cap (`--exact-max-size`, 100 tasks). Measured 2026-11-10 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — a snapshot, not a statistical study.
<!-- results:end -->

The rows are a sample of each 525-instance data set, not an aggregate result:

```sh
cd benchmarks/assembly-line-balancing
for n in 1 125 250 375 500; do set -- "$@" --instance "instance_n=100_$n"; done
uv run --project cpsat python -m bench.run_bench --dataset large "$@" --seconds 30
```

`--dataset` is `small` (n=20), `medium` (n=50), `medium-permuted`, `large`
(n=100) or `very-large` (n=1000); `--all` runs a whole data set.

# PDPTW public comparison

Source: the 100-customer Li & Lim set from SINTEF's TOP benchmark page
(`https://www.sintef.no/projectweb/top/pdptw/li-lim-benchmark/`, archive
`pdp_100.zip`, SHA-256 pinned): 56 instances in six classes (`lc1`, `lc2`,
`lr1`, `lr2`, `lrc1`, `lrc2`) of about 50 requests each.

Li & Lim measures continuous Euclidean distance in abstract time units, while
the local contracts count whole seconds. One Li & Lim unit is taken as ten
seconds, so both backends get the same travel matrix, rounded to a tenth of a
unit, and the 10-unit service time of the `lr` and `lrc` classes is 100
seconds. (The unassigned penalty counts whole service minutes: at one second
per unit a request of those classes could be left out for free.) The result is
therefore a backend comparison on the official coordinates and constraints,
not a bit-identical reproduction of the published two-decimal objective.

Both local backends use the same comparison objective: assign every request,
then minimize travel distance. Vehicle count is reported as a route
diagnostic, but is not encoded as a giant fixed cost: that scalarization creates
an artificial objective cliff for a local-search solver. Reproducing the
benchmark's fleet-count-first objective requires a real lexicographic or two-stage solve.
The best-known solutions on SINTEF's page (`BEST_KNOWN` in the runner, read on
2026-11-10) rank by fewest vehicles first and use unrounded distances, so they
are shown as a reference: a plan with more vehicles can be shorter than the
best known.

```bash
cd benchmarks/pickup-delivery/cpsat
# engine and CP-SAT on the first ten request pairs of one instance
uv run pickup-delivery-public-bench --instance lc101 --requests 10 --seconds 10 --workers 8
# the first instance of each class at 10 requests, 25 requests and whole
uv run pickup-delivery-public-bench --instance lc101 --instance lc201 --instance lr101 \
  --instance lr201 --instance lrc101 --instance lrc201 \
  --requests 10 --requests 25 --requests 0 --seconds 30 --workers 8
# the engine on all 56 whole instances
uv run pickup-delivery-public-bench --all --seconds 30 --backend engine
```

`--instance` and `--requests` repeat; `--requests 0` takes every pair of the
source file, `--all` runs every instance of the archive (whole, unless
`--requests` says otherwise), `--json` prints the results as JSON (progress
goes to stderr), and `--out FILE` rewrites FILE with the results after every
instance, so an interrupted sweep keeps what it measured. A run records its outcome (`ok`, `no solution`, `incomplete`,
`infeasible`) and the budget, seed and workers it was given. The engine always runs
single-threaded; `--workers` is CP-SAT's. The CP-SAT circuit model grows
quadratically per vehicle, so a whole instance (about 50 requests, 25 vehicles)
is past what it solves in 30 s.

<!-- results:start -->
Each plan is re-verified independently: every request served, pickup before dropoff on the
same vehicle, capacity and every time window. Distance is the Li & Lim round trip, with every
leg rounded to a tenth of a unit; lower is better. Cells are vehicles / distance.

**Engine and CP-SAT** on lc101, lc201, lr101, lr201, lrc101 and lrc201, at their first 10 and 25 requests and whole:

| Instance | Requests | engine | CP-SAT |
| --- | --- | --- | --- |
| lc101 | first 10 of 53 requests | **2 / 155.4** | **2 / 155.4** |
| lc101 | first 25 of 53 requests | **5 / 360.1** | **5 / 360.1** |
| lc101 | all 53 requests | **11 / 892** | incomplete (leaves stops unserved) |
| lc201 | first 10 of 51 requests | **2 / 304.9** | **2 / 304.9** |
| lc201 | first 25 of 51 requests | **3 / 444.7** | **3 / 444.7** |
| lc201 | all 51 requests | **4 / 651.1** | incomplete (leaves stops unserved) |
| lr101 | first 10 of 53 requests | **8 / 553** | **8 / 553** |
| lr101 | first 25 of 53 requests | **12 / 1,015.9** | **12 / 1,015.9** |
| lr101 | all 53 requests | **19 / 1,650.8** | no solution in time |
| lr201 | first 10 of 51 requests | **3 / 332.6** | **3 / 332.6** |
| lr201 | first 25 of 51 requests | **6 / 736.6** | 9 / 909.3 |
| lr201 | all 51 requests | **5 / 1,283.7** | incomplete (leaves stops unserved) |
| lrc101 | first 10 of 53 requests | **4 / 425.7** | **4 / 425.7** |
| lrc101 | first 25 of 53 requests | **10 / 1,085.1** | 10 / 1,095.6 |
| lrc101 | all 53 requests | **17 / 1,842.4** | no solution in time |
| lrc201 | first 10 of 51 requests | **2 / 315.1** | **2 / 315.1** |
| lrc201 | first 25 of 51 requests | **4 / 958.8** | 5 / 1,306.1 |
| lrc201 | all 51 requests | **7 / 1,682.9** | incomplete (leaves stops unserved) |

**Engine on 56 whole instances**, beside the best-known solution (which ranks by
fewest vehicles first, then distance, on unrounded distances — a reference, not this
harness's objective):

| Instance | Requests | Best known | engine | Distance vs best known |
| --- | ---: | --- | --- | ---: |
| lc101 | 53 | 10 / 828.94 | 11 / 892 | +7.6% |
| lc102 | 53 | 10 / 828.94 | 10 / 828.7 | 0.0% |
| lc103 | 52 | 9 / 1,035.35 | 10 / 827.5 | -20.1% |
| lc104 | 53 | 9 / 860.01 | 10 / 818.4 | -4.8% |
| lc105 | 53 | 10 / 828.94 | 10 / 828.7 | 0.0% |
| lc106 | 53 | 10 / 828.94 | 11 / 856.7 | +3.3% |
| lc107 | 53 | 10 / 828.94 | 10 / 828.7 | 0.0% |
| lc108 | 53 | 10 / 826.44 | 10 / 904.8 | +9.5% |
| lc109 | 53 | 9 / 1,000.6 | 10 / 827.6 | -17.3% |
| lc201 | 51 | 3 / 591.56 | 4 / 651.1 | +10.1% |
| lc202 | 51 | 3 / 591.56 | 3 / 591.6 | 0.0% |
| lc203 | 51 | 3 / 591.17 | 3 / 591.1 | 0.0% |
| lc204 | 51 | 3 / 590.6 | 4 / 693 | +17.3% |
| lc205 | 51 | 3 / 588.88 | 3 / 588.9 | 0.0% |
| lc206 | 51 | 3 / 588.49 | 4 / 673 | +14.4% |
| lc207 | 51 | 3 / 588.29 | 3 / 588.1 | 0.0% |
| lc208 | 51 | 3 / 588.32 | 4 / 662.5 | +12.6% |
| lr101 | 53 | 19 / 1,650.8 | 19 / 1,650.8 | 0.0% |
| lr102 | 55 | 17 / 1,487.57 | 18 / 1,562.8 | +5.1% |
| lr103 | 52 | 13 / 1,292.68 | 15 / 1,420.3 | +9.9% |
| lr104 | 52 | 9 / 1,013.39 | 12 / 1,236.4 | +22.0% |
| lr105 | 53 | 14 / 1,377.11 | 17 / 1,512.9 | +9.9% |
| lr106 | 52 | 12 / 1,252.62 | 13 / 1,343 | +7.2% |
| lr107 | 52 | 10 / 1,111.31 | 11 / 1,147.4 | +3.2% |
| lr108 | 50 | 9 / 968.97 | 9 / 969.1 | 0.0% |
| lr109 | 53 | 11 / 1,208.96 | 13 / 1,295.8 | +7.2% |
| lr110 | 52 | 10 / 1,159.35 | 14 / 1,334.1 | +15.1% |
| lr111 | 54 | 10 / 1,108.9 | 13 / 1,260.3 | +13.7% |
| lr112 | 53 | 9 / 1,003.77 | 12 / 1,189.7 | +18.5% |
| lr201 | 51 | 4 / 1,253.23 | 5 / 1,283.7 | +2.4% |
| lr202 | 50 | 3 / 1,197.67 | 5 / 1,403.4 | +17.2% |
| lr203 | 51 | 3 / 949.4 | 3 / 949.2 | 0.0% |
| lr204 | 50 | 2 / 849.05 | 4 / 1,008.2 | +18.7% |
| lr205 | 51 | 3 / 1,054.02 | 5 / 1,240.5 | +17.7% |
| lr206 | 50 | 3 / 931.63 | 4 / 989.3 | +6.2% |
| lr207 | 51 | 2 / 903.06 | 3 / 966.1 | +7.0% |
| lr208 | 50 | 2 / 734.85 | 3 / 830.8 | +13.1% |
| lr209 | 51 | 3 / 930.59 | 6 / 1,058.4 | +13.7% |
| lr210 | 51 | 3 / 964.22 | 5 / 1,116 | +15.7% |
| lr211 | 50 | 2 / 911.52 | 4 / 1,036.7 | +13.7% |
| lrc101 | 53 | 14 / 1,708.8 | 17 / 1,842.4 | +7.8% |
| lrc102 | 53 | 12 / 1,558.07 | 17 / 1,744.4 | +12.0% |
| lrc103 | 53 | 11 / 1,258.74 | 13 / 1,427.1 | +13.4% |
| lrc104 | 54 | 10 / 1,128.4 | 11 / 1,195.8 | +6.0% |
| lrc105 | 54 | 13 / 1,637.62 | 15 / 1,694.6 | +3.5% |
| lrc106 | 53 | 11 / 1,424.73 | 13 / 1,487.7 | +4.4% |
| lrc107 | 53 | 11 / 1,230.14 | 13 / 1,346.3 | +9.4% |
| lrc108 | 52 | 10 / 1,147.43 | 12 / 1,275.5 | +11.2% |
| lrc201 | 51 | 4 / 1,406.94 | 7 / 1,682.9 | +19.6% |
| lrc202 | 51 | 3 / 1,374.27 | 7 / 1,555.5 | +13.2% |
| lrc203 | 51 | 3 / 1,089.07 | 4 / 1,126.5 | +3.4% |
| lrc204 | 51 | 3 / 818.66 | 4 / 851 | +4.0% |
| lrc205 | 51 | 4 / 1,302.2 | 6 / 1,496.1 | +14.9% |
| lrc206 | 51 | 3 / 1,159.03 | 5 / 1,226.3 | +5.8% |
| lrc207 | 51 | 3 / 1,062.05 | 3 / 1,091.4 | +2.8% |
| lrc208 | 51 | 3 / 852.76 | 5 / 1,024.9 | +20.2% |

Measured 2026-11-10 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 0. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — a snapshot, not a statistical study.
<!-- results:end -->

The independent scorer verifies every stop exactly once, same vehicle,
pickup-before-delivery, running load, all time windows and depot return time.

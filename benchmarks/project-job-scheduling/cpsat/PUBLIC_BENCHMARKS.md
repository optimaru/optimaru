# RCPSP public comparison

Instances: Pat1-Pat5 of the RG30 set (Vanhoucke et al.), 30 activities and
four renewable resources each, in Patterson format. They are downloaded from a
small third-party archive (`https://www.hexaly.com/examples/rcpsp.zip`),
because the original RCPLIB release is a large archive whose older versions are
withdrawn. That archive is rebuilt from time to time, so the SHA-256 of each
of the five instance files is pinned rather than the archive's. Every activity
duration, precedence arc, renewable resource demand and capacity is mapped
directly.

```bash
cd benchmarks/project-job-scheduling/cpsat
uv run project-job-scheduling-public-bench --instance Pat1 --seconds 10 --workers 8
uv run project-job-scheduling-public-bench --all --seconds 30 --workers 8     # Pat1-Pat5
# the generated instances of the same shape: 30 to 120 activities, five seeds each
uv run project-job-scheduling-public-bench --synthetic 30 --synthetic 60 --synthetic 90 --synthetic 120 \
  --synthetic-seeds 0,1,2,3,4 --seconds 30 --workers 8
```

`--instance` repeats, `--all` runs the five instances, `--synthetic N` (repeatable) generates
instances of N activities in the shape of RG30 (`synthetic_instance`), one per seed of
`--synthetic-seeds` (comma-separated, default 0), `--json` prints the
results as JSON (progress goes to stderr), and `--out FILE` rewrites FILE with
the results after every instance. The engine runs one start
here, on one thread; `--workers` is CP-SAT's.

<!-- results:start -->
Makespan of each solver's schedule, re-verified independently (durations, precedences and
cumulative resource use at every tick). Lower is better; the best value is bold.

| Instance | Activities | Critical path | engine | CP-SAT |
| --- | ---: | ---: | ---: | ---: |
| Pat1 | 30 | 20 | 66 | **62** |
| Pat2 | 30 | 27 | 82 | **74** |
| Pat3 | 30 | 20 | 59 | **56** |
| Pat4 | 30 | 30 | 76 | **73** |
| Pat5 | 30 | 24 | 75 | **71** |

**Generated instances of the same shape** (`--synthetic N`, instance seeds 0, 1, 2, 3 and 4):
four renewable resources of capacity 10, activities of 1 to 10 time units that each use three
of them, and a wide, shallow precedence network (a layer per eight activities).

| Instance | Activities | Critical path | engine | CP-SAT |
| --- | ---: | ---: | ---: | ---: |
| synthetic-30-s0 | 30 | 27 | 84 | **75** |
| synthetic-30-s1 | 30 | 27 | 83 | **73** |
| synthetic-30-s2 | 30 | 23 | 83 | **77** |
| synthetic-30-s3 | 30 | 25 | 101 | **93** |
| synthetic-30-s4 | 30 | 22 | 72 | **62** |
| synthetic-60-s0 | 60 | 54 | 212 | **168** |
| synthetic-60-s1 | 60 | 59 | 184 | **154** |
| synthetic-60-s2 | 60 | 55 | 207 | **161** |
| synthetic-60-s3 | 60 | 57 | 245 | **194** |
| synthetic-60-s4 | 60 | 45 | 196 | **160** |
| synthetic-90-s0 | 90 | 80 | 316 | **242** |
| synthetic-90-s1 | 90 | 86 | 333 | **250** |
| synthetic-90-s2 | 90 | 91 | 334 | **275** |
| synthetic-90-s3 | 90 | 78 | 313 | **248** |
| synthetic-90-s4 | 90 | 71 | 290 | **250** |
| synthetic-120-s0 | 120 | 108 | 412 | **356** |
| synthetic-120-s1 | 120 | 120 | 444 | **377** |
| synthetic-120-s2 | 120 | 105 | 433 | **338** |
| synthetic-120-s3 | 120 | 113 | 408 | **342** |
| synthetic-120-s4 | 120 | 97 | 388 | **325** |

Measured 2026-11-12 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 0. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — a snapshot, not a statistical study.
<!-- results:end -->

The comparison exposed and fixed an exact-backend scaling issue: its old time
horizon expanded Pat1 to 7,360 ticks although a 161-tick serial schedule is an
upper bound. The tighter safe horizon plus strict benchmark capacity cuts make
the comparison finish within the shared budget. The independent scorer
rechecks duration, precedence and cumulative resource usage at every tick.

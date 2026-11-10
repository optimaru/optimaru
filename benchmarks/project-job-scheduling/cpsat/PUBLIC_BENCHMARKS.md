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
uv run project-job-public-bench --instance Pat1 --seconds 10 --workers 8
uv run project-job-public-bench --all --seconds 30 --workers 8     # Pat1-Pat5
```

`--instance` repeats, `--all` runs the five instances, and `--json` prints the
results as JSON (progress goes to stderr). The engine always runs
single-threaded; `--workers` is CP-SAT's.

<!-- results:start -->
Makespan of each solver's schedule, re-verified independently (durations, precedences and
cumulative resource use at every tick). Lower is better; the best value is bold.

| Instance | Activities | Critical path | engine | CP-SAT |
| --- | ---: | ---: | ---: | ---: |
| Pat1 | 30 | 20 | 66 | **62** |
| Pat2 | 30 | 27 | 82 | **74** |
| Pat3 | 30 | 20 | 59 | **55** |
| Pat4 | 30 | 30 | 76 | **73** |
| Pat5 | 30 | 24 | 75 | **71** |

Measured 2026-11-10 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 0. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — a snapshot, not a statistical study.
<!-- results:end -->

The comparison exposed and fixed an exact-backend scaling issue: its old time
horizon expanded Pat1 to 7,360 ticks although a 161-tick serial schedule is an
upper bound. The tighter safe horizon plus strict benchmark capacity cuts make
the comparison finish within the shared budget. The independent scorer
rechecks duration, precedence and cumulative resource usage at every tick.

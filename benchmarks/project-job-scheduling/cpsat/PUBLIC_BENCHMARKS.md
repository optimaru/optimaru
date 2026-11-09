# RCPSP public comparison

Instances: Pat1-Pat5 of the RG30 set (Vanhoucke et al.), 30 activities and
four renewable resources each, in Patterson format. They are downloaded from a
small SHA-256-pinned third-party archive
(`https://www.hexaly.com/examples/rcpsp.zip`), because the original RCPLIB
release is a large archive whose older versions are withdrawn. Every activity duration,
precedence arc, renewable resource demand and capacity is mapped directly.

```bash
uv run --project benchmarks/project-job-scheduling/cpsat \
  python -m project_job_cpsat.public_benchmark \
  --instance Pat1 --seconds 10 --backend both
```

Observed locally on 2026-08-17 with one worker and seed 0:

| Backend | Makespan | Resource excess | Feasible |
|---|---:|---:|---:|
| Engine | 67 | 0 | yes |
| CP-SAT | 67 | 0 | yes |

The comparison exposed and fixed an exact-backend scaling issue: its old time
horizon expanded Pat1 to 7,360 ticks although a 161-tick serial schedule is an
upper bound. The tighter safe horizon plus strict benchmark capacity cuts make
the comparison finish within the shared budget. The independent scorer
rechecks duration, precedence and cumulative resource usage at every tick.

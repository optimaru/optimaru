# Tournament scheduling benchmark

Transcribes the Timefold Quickstarts `tournament-scheduling` example's
demo data generator (Apache-2.0), with modifications; see the repository [NOTICE](../../NOTICE).

The [`tournament-scheduling`](../../docs/use-cases/tournament-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `tournament_scheduling_cpsat`). The CP-SAT model implements the
same four constraints under the same names, with the defaults read from the engine's
`constraint_info()`, and both answers are rescored by the use case's own evaluator.
Only the two hard constraints (`team_conflict_cost`, `team_unavailability_cost`) are
raw-identical to the engine; the two imbalance constraints
(`assignment_count_imbalance_cost`, `confrontation_count_imbalance_cost`) minimize an
integral variance surrogate (`scale * ssd`) instead of the engine's
`round6(sqrt(ssd))`, and the confrontation surrogate counts a pair once per shared
day. Exactly where the two paths agree and where they do not, and the tests that
pin both down: [`cpsat/README.md`](cpsat/README.md#constraints-and-parity-with-the-engine).

```sh
cd benchmarks/tournament-scheduling/cpsat
OPTIMARU_USE_CASES=tournament_scheduling uv sync   # also builds this checkout's optimaru (tournament_scheduling only)
uv run pytest -q                       # benchmark harness tests
uv run tournament-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded. CP-SAT gets `--workers` search workers;
its budget starts once its model is built, and the time the runner reports
for it includes building the model.

Both solvers end at the same score on the eight instances of the two smallest sizes
(`small`, `demo` and their other seeds), where the optimum is easy to reach; the larger
sizes are the ones that tell the solvers apart.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| small | 48 slots / 7 teams / 12 days | **0hard/0medium/-1.5492soft** | **0hard/0medium/-1.5492soft** |
| small-2 | 48 slots / 7 teams / 12 days | **0hard/0medium/-1.5492soft** | **0hard/0medium/-1.5492soft** |
| small-3 | 48 slots / 7 teams / 12 days | **0hard/0medium/-1.5492soft** | **0hard/0medium/-1.5492soft** |
| small-4 | 48 slots / 7 teams / 12 days | **0hard/0medium/-1.5492soft** | **0hard/0medium/-1.5492soft** |
| demo | 72 slots / 7 teams / 18 days | **0hard/0medium/-1.5492soft** | **0hard/0medium/-1.5492soft** |
| demo-2 | 72 slots / 7 teams / 18 days | **0hard/0medium/-1.5492soft** | **0hard/0medium/-1.5492soft** |
| demo-3 | 72 slots / 7 teams / 18 days | **0hard/0medium/-1.5492soft** | **0hard/0medium/-1.5492soft** |
| demo-4 | 72 slots / 7 teams / 18 days | **0hard/0medium/-1.5492soft** | **0hard/0medium/-1.5492soft** |
| medium | 96 slots / 8 teams / 24 days | **0hard/0medium/-1.8516soft** | **0hard/0medium/-1.8516soft** |
| medium-2 | 96 slots / 8 teams / 24 days | 0hard/0medium/-1.8974soft | **0hard/0medium/-1.8516soft** |
| medium-3 | 96 slots / 8 teams / 24 days | **0hard/0medium/-1.8516soft** | **0hard/0medium/-1.8516soft** |
| medium-4 | 96 slots / 8 teams / 24 days | **0hard/0medium/-1.8516soft** | **0hard/0medium/-1.8516soft** |
| large | 180 slots / 10 teams / 36 days | **0hard/0medium/0soft** | 0hard/0medium/-2soft |
| large-2 | 180 slots / 10 teams / 36 days | **0hard/0medium/0soft** | 0hard/0medium/-2soft |
| large-3 | 180 slots / 10 teams / 36 days | **0hard/0medium/0soft** | 0hard/0medium/-2soft |
| larger | 360 slots / 14 teams / 60 days | **0hard/0medium/0soft** | 0hard/-1medium/-690316.2371soft |
| larger-2 | 360 slots / 14 teams / 60 days | **0hard/0medium/0soft** | 0hard/-2medium/-976107.6747soft |
| larger-3 | 360 slots / 14 teams / 60 days | **0hard/0medium/0soft** | 0hard/-3medium/-854507.4095soft |
| xlarge | 800 slots / 20 teams / 100 days | **0hard/0medium/-5.1640soft** | -1120hard/-275medium/-945259.0250soft |
| xlarge-2 | 800 slots / 20 teams / 100 days | **0hard/0medium/-6.0698soft** | -451hard/-57medium/-497911.1810soft |

Measured 2026-11-12 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
Note: the two imbalance costs are sqrt(ssd) rounded to 6 significant figures; CP-SAT minimises the integral surrogate scale * ssd instead (not raw-identical), so both answers are rescored by the engine.
<!-- results:end -->

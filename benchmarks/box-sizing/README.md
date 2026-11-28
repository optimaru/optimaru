# Box sizing benchmark

The [`box-sizing`](../../docs/use-cases/box-sizing) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `box_sizing_cpsat`). Both solvers minimise the same objective under
the same rules, and both answers are rescored by the use case's own evaluator. The CP-SAT model is
this project's own ([`cpsat/box_sizing_cpsat/model.py`](cpsat/box_sizing_cpsat/model.py)), with its
tests beside it: the project does not install external-solver.

```sh
cd benchmarks/box-sizing/cpsat
OPTIMARU_USE_CASES=box_sizing uv sync   # also builds this checkout's optimaru (box_sizing only)
uv run pytest -q                        # the model against the evaluator and an enumeration, the ladder
uv run box-sizing-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The ladder is the use case's demo instance and 19 generated warehouses of five sizes: the
demo's parcels (`optimaru.box_sizing.demo`) for a seed, against a seeded list of candidate
boxes whose largest takes every parcel.

The engine runs single-threaded. CP-SAT gets `--workers` search workers;
its budget starts once its model is built, and the time the runner reports
for it includes building the model.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| small | 40 parcels / 12 boxes / 3 types | **0hard/-587781250soft** | **0hard/-587781250soft** |
| small-2 | 40 parcels / 12 boxes / 3 types | **0hard/-1105528125soft** | **0hard/-1105528125soft** |
| small-3 | 40 parcels / 12 boxes / 3 types | **0hard/-1998754625soft** | **0hard/-1998754625soft** |
| demo | 120 parcels / 24 boxes / 6 types | **0hard/-2767496500soft** | **0hard/-2767496500soft** |
| medium | 300 parcels / 40 boxes / 6 types | **0hard/-6212320500soft** | **0hard/-6212320500soft** |
| medium-2 | 300 parcels / 40 boxes / 6 types | **0hard/-4737729250soft** | **0hard/-4737729250soft** |
| medium-3 | 300 parcels / 40 boxes / 6 types | **0hard/-9273233125soft** | **0hard/-9273233125soft** |
| medium-4 | 300 parcels / 40 boxes / 6 types | **0hard/-7020637375soft** | **0hard/-7020637375soft** |
| large | 1000 parcels / 80 boxes / 8 types | **0hard/-17595527000soft** | 0hard/-18117095000soft |
| large-2 | 1000 parcels / 80 boxes / 8 types | **0hard/-14207278750soft** | **0hard/-14207278750soft** |
| large-3 | 1000 parcels / 80 boxes / 8 types | **0hard/-16004120625soft** | 0hard/-16510221625soft |
| large-4 | 1000 parcels / 80 boxes / 8 types | **0hard/-17142974125soft** | **0hard/-17142974125soft** |
| xlarge | 3000 parcels / 150 boxes / 10 types | **0hard/-57295802625soft** | 0hard/-62669618625soft |
| xlarge-2 | 3000 parcels / 150 boxes / 10 types | **0hard/-45055190250soft** | 0hard/-49120153250soft |
| xlarge-3 | 3000 parcels / 150 boxes / 10 types | **0hard/-44929857625soft** | 0hard/-49920395625soft |
| xlarge-4 | 3000 parcels / 150 boxes / 10 types | **0hard/-54051110125soft** | 0hard/-56654590125soft |
| huge | 8000 parcels / 300 boxes / 12 types | **0hard/-116005743625soft** | 0hard/-154516914625soft |
| huge-2 | 8000 parcels / 300 boxes / 12 types | **0hard/-117130812250soft** | 0hard/-175047767250soft |
| huge-3 | 8000 parcels / 300 boxes / 12 types | **0hard/-122178402000soft** | 0hard/-140974789000soft |
| huge-4 | 8000 parcels / 300 boxes / 12 types | **0hard/-131846595875soft** | 0hard/-158796992875soft |

Measured 2026-11-28 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 4 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

# Container loading benchmark

The [`container-loading`](../../docs/use-cases/container-loading) use case against an
**OR-Tools CP-SAT** model ([`cpsat/`](cpsat), package `container_loading_cpsat`), on
instances whose fewest containers is known: containers cut into boxes.

The two solvers do not search the same space. The engine decides loading orders, and its
decoder derives where the items stand. The CP-SAT model decides the positions themselves.
Every loading the engine can decode is a solution of the CP-SAT model, so:

- a **proven** CP-SAT optimum is a bound no loading order can beat, and an engine answer
  that equals it is optimal;
- an engine answer that is worse than a proven optimum shows what the decoder or the
  search leaves behind;
- where CP-SAT proves nothing in the budget (from about 20 items on), its answer is just
  another loading, and the instance's known optimum (in the Size column) is the yardstick.

Neither answer is scored by the engine's decoder. Both are turned into a solved model,
checked as a loading (inside the container, no overlap, enough support, an allowed
orientation) and scored from the positions alone by
[`container_loading_cpsat.evaluator`](cpsat/container_loading_cpsat/evaluator/__init__.py),
with the engine's own default weights. For the engine's answers the runner also compares
that score with the one the engine reported.

```sh
cd benchmarks/container-loading/cpsat
OPTIMARU_USE_CASES=container_loading uv sync   # also builds this checkout's optimaru (container_loading only)
uv run pytest -q                           # the model, the evaluator and the instances
uv run container-loading-compare --seconds 10 --workers 8   # engine vs CP-SAT (--markdown, --json)
uv run container-loading-known-optimum --seconds 10         # the engine alone, larger instances
uv run container-loading-vehicle-loads --seconds 10 30      # the engine alone, trucks with many stops (--warehouse)
```

What the CP-SAT model covers, and what it refuses:
[`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded. CP-SAT gets `--workers` search workers; its budget
starts once its model is built, and the time the runner reports for it includes building
the model. The model is built for up to 50 items; a larger instance is reported as
`TOO_LARGE` for CP-SAT.

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| tiny | 5 items / 3 containers (optimum 1) | **0hard/-1soft** | **0hard/-1soft** |
| tiny-2 | 5 items / 3 containers (optimum 1) | **0hard/-1soft** | **0hard/-1soft** |
| tiny-3 | 5 items / 3 containers (optimum 1) | **0hard/-1soft** | **0hard/-1soft** |
| tiny-4 | 5 items / 3 containers (optimum 1) | **0hard/-1soft** | **0hard/-1soft** |
| small | 8 items / 4 containers (optimum 2) | **0hard/-2soft** | **0hard/-2soft** |
| small-2 | 8 items / 4 containers (optimum 2) | **0hard/-2soft** | **0hard/-2soft** |
| small-3 | 8 items / 4 containers (optimum 2) | **0hard/-2soft** | **0hard/-2soft** |
| small-4 | 8 items / 4 containers (optimum 2) | **0hard/-2soft** | **0hard/-2soft** |
| medium | 15 items / 5 containers (optimum 3) | **0hard/-3soft** | **0hard/-3soft** |
| medium-2 | 15 items / 5 containers (optimum 3) | **0hard/-3soft** | **0hard/-3soft** |
| medium-3 | 15 items / 5 containers (optimum 3) | **0hard/-3soft** | **0hard/-3soft** |
| medium-4 | 15 items / 5 containers (optimum 3) | **0hard/-3soft** | **0hard/-3soft** |
| large | 32 items / 6 containers (optimum 4) | **0hard/-4soft** | 0hard/-5soft |
| large-2 | 32 items / 6 containers (optimum 4) | **0hard/-5soft** | **0hard/-5soft** |
| large-3 | 32 items / 6 containers (optimum 4) | **0hard/-5soft** | **0hard/-5soft** |
| large-4 | 32 items / 6 containers (optimum 4) | **0hard/-4soft** | 0hard/-5soft |
| xlarge | 500 items / 30 containers (optimum 20) | **0hard/-21soft** | — (TOO_LARGE) |
| xlarge-2 | 500 items / 30 containers (optimum 20) | **0hard/-21soft** | — (TOO_LARGE) |
| xlarge-3 | 500 items / 30 containers (optimum 20) | **0hard/-21soft** | — (TOO_LARGE) |
| xlarge-4 | 500 items / 30 containers (optimum 20) | **0hard/-21soft** | — (TOO_LARGE) |

Measured 2026-11-28 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 4 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

Every instance is `perfect_packing(containers, boxes per container, seed)` of
[`instances.py`](cpsat/container_loading_cpsat/instances.py) with containers of
12 x 8 x 8: `tiny` 1 x 5, `small` 2 x 4, `medium` 3 x 5, `large` 4 x 8 and `xlarge`
20 x 25, four seeds each, with half as many spare containers again (at least two). The
soft score is minus the number of containers used, so a row at the optimum reads
`0hard/-<optimum>soft`.

## Where the fewest containers is known

`container-loading-known-optimum` runs the engine alone on larger instances of the same
kind: containers of 1200 x 800 x 1000 cut into boxes with sides in steps of 100, three
seeds per row, 10 s per solve, one thread. "Slack" is the share of the volume taken out
again after the cut, so that the load is not full to the last unit. "Volume needs" is the
number of containers the items' volume fills, rounded up: without slack it is the optimum
(the number cut), with slack a lower bound, the number cut being known to be enough.
"Construction" is what the first solution uses (`time_limit_seconds=0`), "solve" what the
solve returns. Where the three seeds differ they are listed in seed order.

| cut containers x boxes | slack | rotations | items | volume needs | construction | solve |
| --- | --- | --- | --- | --- | --- | --- |
| 4 x 15 | 0% | fixed | 60 | 4 | 6 (1 unplaced) | 5 |
| 4 x 15 | 0% | all | 60 | 4 | 5 | 5 |
| 4 x 30 | 0% | all | 120 | 4 | 5 | 5 |
| 10 x 20 | 0% | fixed | 200 | 10 | 13/13/12 | 12 |
| 10 x 20 | 0% | all | 200 | 10 | 11/12/11 | 11 |
| 20 x 25 | 0% | all | 500 | 20 | 21 | 21 |
| 4 x 15 | 10% | all | 54/55/53 | 4 | 5 | 4 |
| 10 x 20 | 10% | fixed | 178/178/179 | 10/9/10 | 12/11/12 | 11/11/10 |
| 10 x 20 | 10% | all | 178/178/179 | 10/9/10 | 10 | 10 |
| 20 x 25 | 10% | all | 446/449/450 | 19/19/18 | 20/19/19 | 19 |
| 40 x 50 | 10% | all | 1803/1801/1801 | 36/37/37 | 37 | 37 |

Measured 2026-11-28 on one Apple M4 Max Mac (16 cores), while other jobs were running on
the machine. Single runs: treat a difference of one container as noise.

## Vehicle loads

`container-loading-vehicle-loads`
([`vehicle_loads.py`](cpsat/container_loading_cpsat/vehicle_loads.py)) runs the engine alone
on one cargo space with many stops, every box's `unload_order` its stop: *catalog* trucks
(cartons of nine sizes, most upright, a tenth with a `max_load_on_top`; whether every order
and limit can be met is not known) and *cut* trucks (the space cut into one slab per stop,
each slab into boxes, half the volume taken out again from the top; the positions they were
cut into meet every order and limit, which `tests/test_benchmark.py` checks). Three seeds per
row, one thread per solve, the default weights (`--warehouse`: the weights a warehouse
system sends, `vehicle_loads.warehouse_weights()`):

| truck | budget | hard 0 | pairs left (mean) | unplaced (mean) | first hard 0 (s) |
| --- | --- | --- | --- | --- | --- |
| catalog 2.5t, 12 stops, 120 boxes | 5 s | 1/3 | 0 | 3.0 | 1.7 |
| catalog 2.5t, 12 stops, 120 boxes | 10 s | 1/3 | 0 | 3.0 | 1.7 |
| catalog 2.5t, 12 stops, 120 boxes | 30 s | 1/3 | 0 | 3.0 | 1.8 |
| catalog 5t, 15 stops, 200 boxes | 5 s | 2/3 | 0 | 0.3 | 2.1/0.3 |
| catalog 5t, 15 stops, 200 boxes | 10 s | 3/3 | 0 | 0.0 | 2.1/0.3/5.9 |
| catalog 5t, 15 stops, 200 boxes | 30 s | 3/3 | 0 | 0.0 | 2.1/0.3/5.5 |
| catalog 5t, 20 stops, 250 boxes | 5 s | 0/3 | 101 | 8.3 | - |
| catalog 5t, 20 stops, 250 boxes | 10 s | 0/3 | 71 | 16.3 | - |
| catalog 5t, 20 stops, 250 boxes | 30 s | 0/3 | 15 | 10.7 | - |
| catalog 5t, 25 stops, 220 boxes | 5 s | 0/3 | 58 | 13.7 | - |
| catalog 5t, 25 stops, 220 boxes | 10 s | 0/3 | 26 | 11.3 | - |
| catalog 5t, 25 stops, 220 boxes | 30 s | 0/3 | 26 | 11.3 | - |
| catalog 5t, 20 stops, 300 boxes | 5 s | 0/3 | 335 | 38.7 | - |
| catalog 5t, 20 stops, 300 boxes | 10 s | 0/3 | 309 | 48.3 | - |
| catalog 5t, 20 stops, 300 boxes | 30 s | 0/3 | 151 | 54.7 | - |
| cut 2.5t, 10 stops | 5 s | 3/3 | 0 | 0.0 | 0.2/0.0/0.4 |
| cut 2.5t, 10 stops | 10 s | 3/3 | 0 | 0.0 | 0.2/0.0/0.3 |
| cut 2.5t, 10 stops | 30 s | 3/3 | 0 | 0.0 | 0.2/0.0/0.3 |
| cut 5t, 15 stops | 5 s | 0/3 | 79 | 15.0 | - |
| cut 5t, 15 stops | 10 s | 0/3 | 223 | 28.0 | - |
| cut 5t, 15 stops | 30 s | 1/3 | 186 | 16.3 | 18.4 |
| cut 5t, 20 stops | 5 s | 0/3 | 255 | 40.7 | - |
| cut 5t, 20 stops | 10 s | 0/3 | 255 | 40.7 | - |
| cut 5t, 20 stops | 30 s | 0/3 | 255 | 40.7 | - |

Measured 2026-11-28 on one Apple M4 Max Mac (16 cores), four solves at a time, while other
jobs were running on the machine. 0.9.0 met every constraint on none of these trucks and
left 150 to 1,730 pairs per truck after 30 s; the guide has the comparison
([How good, how fast](../../docs/use-cases/container-loading/README.md#how-good-how-fast)).

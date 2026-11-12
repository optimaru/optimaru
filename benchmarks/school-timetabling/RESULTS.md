# School timetabling benchmark — results and method

Results and methodology of comparing this repository's C++ solver with
OR-Tools CP-SAT on the same instances and the same wall-clock budget. The benchmark runs continuously in CI through
[`.github/workflows/school-timetabling-bench.yml`](../../.github/workflows/school-timetabling-bench.yml)
(push / PR), and the harness lives in [this directory](./).

## Method

There are three tracks. Track 1 measures how well the solvers handle "the
same six constraints" (pure search performance); track 2 measures the quality
of the C++ canonical solver against CP-SAT on the full real-world model; track
3 relaxes `unassigned_lessons_cost` on an instance with one weekday removed and compares how much
each solver fills (partial timetabling; see README.md "Track 3"). CI publishes
all three tables; the tables below cover tracks 1 and 2.

### Track 1 — the quickstart model's six constraints

- **Constraints**: exactly the quickstart model's six — room_conflict_cost /
  teacher_conflict_cost / class_conflict_cost (hard), teacher_room_change_cost /
  teacher_time_efficiency_cost / class_subject_repeat_cost (soft). The
  model's seventh term, teacher_unavailability_cost, gets empty
  data in these instances, so it contributes 0 (fairness).
- **Instances**: `gen_quickstart.py` (in `cpsat/school_timetabling_cpsat/`)
  generates them from `random.Random(seed)` and writes each one twice, as
  JSON for CP-SAT and the evaluator and as a token file for the C++ bench;
  both solvers get the same data. 5 days × 8 periods = 40 slots, a 34-period
  weekly curriculum per class. Presets (classes / teachers / rooms):
  `S: 6/12/7 (204 lessons)`, `M: 12/20/13 (408 lessons)`,
  `L: 24/40/25 (816 lessons)`. Until 2026-11-12 the C++ bench generated the
  instances itself, with `std::shuffle`, whose result depends on the standard
  library: the same presets and seed were another school on macOS than on
  Linux, and are another one again now. Every track-1 table below says which
  instances it was measured on.
- **Participants**: (1) this repository's C++ engine (`st_quality_bench`),
  (2) OR-Tools CP-SAT
  (`cpsat_quickstart.py` — the three hard constraints as constraints, the three
  soft ones as the objective).
- **Fairness rules**:
  - Both solvers get the same instance and the same wall-clock
    budget (10 s by default).
  - C++ runs single-threaded, CP-SAT as an 8-worker portfolio (adjustable
    with `--workers`; same budget, different CPU use — stated in the table
    footnotes).
- **Cross-check gate**: a fixed instance + fixed assignment in which all six
  constraints contribute something non-zero (`gen_quickstart.crosscheck()`) is
  scored by C++ (`st_quality_bench crosscheck`) and the shared evaluator (`evaluate.py`);
  the two must be **identical**
  (`CROSSCHECK_SCORE=-3hard/-3soft`) before the benchmark proceeds.
- **Rescoring**: every solution of both solvers is rescored with the
  shared evaluator `evaluate.py` and compared with its self-report. The table
  shows the rescored score; mismatches are shown as such.

### Track 2 — full canonical constraints, C++ canonical vs CP-SAT

- **Model**: this repository's canonical `TimetableProblem` (every hard and
  soft canonical constraint — blocks, merged parallel sections, team teaching, teacher
  unavailability and weekly/consecutive caps, special-room demand, fixed
  placements). Same as the
  [`API_SPEC.md`](../../docs/use-cases/school-timetabling/API_SPEC.md) contract.
- **Instances**: `gen_canonical.py` generates S (6 classes / 15 teachers / 72
  courses), M (12 / 23 / 138) and L (24 / 42 / 270). Class periods total == 34
  slots, with a teacher-load check and pydantic validation.
- **Participants**: (1) the C++ canonical solver (`optimaru.school_timetabling.solve`,
  single-threaded), (2) the CP-SAT reference (`teacher_scheduling.solve` in
  `cpsat/`, 8 workers). Same time limit, same seed argument.
- **Rescoring**: both backends' solutions are rescored with the CP-SAT
  package's `validate.py` — the hard-violation list + `compute_penalties` raw
  counts × the default weights (20/3/30/10/5/1). **Lower is better** (the
  opposite direction from track 1's soft score).
- That both implementations share the same semantics is pinned separately by
  a parity gate (`cpsat/tests/test_engine_parity.py`, and the 44 hand-computed
  cases of the C++ `st_canonical_test`).
- Engine numbers use the default **monolithic incremental calculator** (fast
  path). The plugin-assembly path with the same semantics
  (`optimaru.school_timetabling.solve(..., constraints=)`) runs at 37–62% of the
  monolith's steps per second (medians by size: three seeds of a step-limited
  solve of S, M and L), so it does not represent these numbers — the benchmark
  and CI always use the fast path. See the plugin section of
  [`docs/use-cases/school-timetabling/python.md`](../../docs/use-cases/school-timetabling/python.md).

## Track 1 on the Python-generated instances (2026-11-12)

The instances of `gen_quickstart.py`, seed 1 — the ones the harness and CI
solve now. Environment: one 4-vCPU x86-64 Linux cloud machine (Intel Xeon,
2.1 GHz), **30 s** per instance per solver, seed 1, one run. CP-SAT is
OR-Tools 9.15. The cross-check gate passed. Soft: higher is better; every
rescore == self-report.

| instance | solver | final (rescored) | first feasible | moves/s |
| --- | --- | --- | --- | --- |
| S: 6 classes/12 teachers/7 rooms | **cpp** | **0hard/175soft** | 1 ms | 8,478,047 |
| S | cpsat (8 workers) | 0hard/-20soft | 6992 ms | n/a |
| S | cpsat (4 workers) | 0hard/124soft | 4640 ms | n/a |
| M: 12 classes/20 teachers/13 rooms | **cpp** | **0hard/350soft** | 5 ms | 8,834,942 |
| M | cpsat (8 workers) | 0hard/-489soft | 20252 ms | n/a |
| M | cpsat (4 workers) | 0hard/-1387soft | 11730 ms | n/a |
| L: 24 classes/40 teachers/25 rooms | **cpp** | **0hard/666soft** | 20 ms | 6,207,261 |
| L | cpsat (8 workers) | n/a (no feasible solution within 30 s, UNKNOWN) | n/a | n/a |
| L | cpsat (4 workers) | n/a (no feasible solution within 30 s, UNKNOWN) | n/a | n/a |

- The harness ran twice, once with `--workers 8` (the default of every table
  here) and once with `--workers 4`, the machine's core count: eight workers
  share four cores on this machine. The cpp rows are of the first run; the
  second run's cpp scores were the same (175 / 350 / 666).
- This machine evaluates about half as many moves per second as the Mac of
  the tables below (8.5M against 16.4M on S) and has a quarter of its cores,
  which costs CP-SAT more than it costs the single-threaded engine. Do not
  read these rows against the tables below: those are of other instances
  (the C++ bench's former generator) and another machine.
- Reproduce: `uv run school-timetabling-compare --sizes S,M,L --seconds 30`
  (`--workers 4` for the second set of rows).

## Latest measurement (local, 2026-11-04)

Environment: one Apple M4 Max Mac (16 cores), **30 s** per instance per
solver, seed 1, one run. CP-SAT is OR-Tools 9.15 (8 workers). The
cross-check gate passed. Track 1 is of the instances the C++ bench generated
until 2026-11-12 (libc++ `std::shuffle`); track 2's instances are unchanged.

### Track 1 (soft: higher is better; every rescore == self-report)

| instance | solver | final (rescored) | first feasible | moves/s |
| --- | --- | --- | --- | --- |
| S: 6 classes/12 teachers/7 rooms | **cpp** | **0hard/175soft** | 0 ms | 16,373,075 |
| S | cpsat | 0hard/164soft | 1598 ms | n/a |
| M: 12 classes/20 teachers/13 rooms | **cpp** | **0hard/350soft** | 3 ms | 16,241,783 |
| M | cpsat | 0hard/-204soft | 6313 ms | n/a |
| L: 24 classes/40 teachers/25 rooms | **cpp** | **0hard/676soft** | 11 ms | 12,091,404 |
| L | cpsat | n/a (no feasible solution within 30 s, UNKNOWN) | n/a | n/a |

### Track 2 (weighted penalty: lower is better; validate.py rescoring)

| instance | backend | status | hard violations | weighted penalty |
| --- | --- | --- | --- | --- |
| canonical_S | cpsat (8 workers) | FEASIBLE | 0 | 320 |
| canonical_S | **engine (1 thread)** | FEASIBLE | 0 | **231** |
| canonical_M | cpsat (8 workers) | FEASIBLE | 0 | 1208 |
| canonical_M | **engine (1 thread)** | FEASIBLE | 0 | **1151** |
| canonical_L | cpsat (8 workers) | FEASIBLE | 0 | 3222 |
| canonical_L | **engine (1 thread)** | FEASIBLE | 0 | **2606** |

The section below is the earlier 10 s measurement that the tuning history
refers to.

## Final measurement (local, 2026-08-12)

Environment: Apple Silicon macOS, 10 s per instance per solver, seed 1, one
run. CP-SAT is OR-Tools 9.15 (8 workers). Absolute values depend on the
machine (CI runs on ubuntu-latest, 4 vCPUs), and a single seed is indicative
only. Track 1 here, and every track-1 figure of the tuning history below, is
of the instances the C++ bench generated until 2026-11-12, as built on
macOS.

### Track 1 (soft: higher is better; every rescore == self-report)

| instance | solver | final (rescored) | first feasible | moves/s |
| --- | --- | --- | --- | --- |
| S: 6 classes/12 teachers/7 rooms | **cpp** | **0hard/175soft** | 0 ms | 13,226,204 |
| S | cpsat | 0hard/78soft | 1566 ms | n/a |
| M: 12 classes/20 teachers/13 rooms | **cpp** | **0hard/348soft** | 3 ms | 12,870,135 |
| M | cpsat | 0hard/-1377soft | 6424 ms | n/a |
| L: 24 classes/40 teachers/25 rooms | **cpp** | **0hard/635soft** | 11 ms | 12,273,466 |
| L | cpsat | n/a (no feasible solution within 10 s, UNKNOWN) | n/a | n/a |

- moves/s: C++ is estimated from the improvement trajectory (one move per
  step). Not applicable to CP-SAT.
- CP-SAT runs 8 parallel workers (portfolio); C++ runs single-threaded.
- This table predates the adoption of the automatic as-move default on
  2026-08-18. Since then L cpp averages 684.8 over 6 seeds (+6.1% over
  645.7) and S/M are unchanged — see "Tuning history" (one machine, per-seed
  values, so the comparison with the table's single seed-1 run is
  indicative only).

### Track 2 (weighted penalty: lower is better; validate.py rescoring; 0 hard violations everywhere)

| instance | backend | status | hard violations | weighted penalty |
| --- | --- | --- | --- | --- |
| canonical_S | cpsat (8 workers) | FEASIBLE | 0 | 443 |
| canonical_S | **engine (1 thread)** | FEASIBLE | 0 | **310** |
| canonical_M | cpsat (8 workers) | FEASIBLE | 0 | 1511 |
| canonical_M | **engine (1 thread)** | FEASIBLE | 0 | **1176** |
| canonical_L | cpsat (8 workers) | FEASIBLE | 0 | 4164 |
| canonical_L | **engine (1 thread)** | FEASIBLE | 0 | **2752** |

- CP-SAT's portfolio search varies between runs (243–443 seen on S). The
  engine's lead is judged on the large M/L gaps (−22% on M, −34% on L in this
  run).
- The engine's 6-seed averages (when tuning was settled, seeds 1–6): S 288.7 /
  M 1131.8 / L 2696.7 — S −22.8% / M −16.7% / L −17.9% against before tuning;
  L feasibility went from 1/3 seeds to 18/18 (hard 0).

### Tuning history

Adopted from ten rounds of measure-then-adopt-or-reject tuning: same-class
nearby selection, an entity recommender aimed at hard violations, a best-of-8
same-class swap move limited to the soft phase, and an automatic scale for the
Late Acceptance size. As a result track-1 C++ soft rose on average by +23.5%
on M and +79.9% on L against before tuning (S stays at its ceiling around
175), with moves/s holding at 12.3M–13.2M. The measurement protocol (track 1
S/M/L × averages over 3–6 seeds, track 2 rescored by validate.py) follows the
fairness rules of [`README.md`](README.md).

Extra round on 2026-08-18: adopted the default that switches the as-move of
"Two search strategies" below on automatically at 0.2/slotRoomCount only in
the measured band of [900, 1300] cells — track-1 L 6-seed average 645.7 →
**684.8 (+6.1%, 6 wins 0 losses)**; S/M unchanged because the rule gives 0
there (10 s per seed, one machine, interleaved A/B).

### Two search strategies — worth it only by size (as-move on automatically only in the measured band)

The two strategies adopted for vehicle routing were extended to the engine's
assignment shape and measured here too. What they share is that they are **a way of
spending steps**, not a move kind.

- `Config::descent_weight` — picks a lesson and a nearby partner, evaluates
  every slotRoom of both plus their swap, applies the best, and repeats until
  nothing improves. Always improving, so it skips the acceptor.
- `Config::ruin_recreate_as_move_weight` — ruins a few lessons, repairs them with
  best-value, scores the result like an ordinary move and lets the acceptor
  judge it; rejected results are undone. Unlike the stagnation kick it is
  neither rare nor unconditional.

On the assignment shape the cost of both strategies is set by the **value range**. One
descent costs ~2 × slotRoomCount evaluations, one destroy-repair
ruinCount × slotRoomCount, so on a 40×25 grid one of them is worth thousands of
ordinary steps. That is why the weight sits around 1e-4 here, while an
employee-rostering domain with a value range of 40 employees sits around 0.02.
Plugging in vehicle routing's 0.01/0.05 at first dropped M from 140 million to
10 million steps and collapsed soft from 349.8 to 263.4.

Keeping the budget share constant means keeping weight × value range
constant. Below, that constant is 0.2 — S 7e-4, M 4e-4, L 2e-4. Track 1,
20 s, 10 seeds, average self-reported soft (higher is better), wins/losses
compared per seed:

| size (lessons/cells) | baseline | as-move (0.2/cells) | W-L-D |
| --- | --- | --- | --- |
| S 204/280 | 175.0 | 172.9 | 0-1-9 |
| M 408/520 | 340.0 | 346.7 (+2.0%) | 5-1-4 |
| L 816/1000 | 653.2 | **680.0 (+4.1%)** | **8-2-0** |

Varying the weight on L alone shows a clear peak: 1e-4 668.1 (7-3), 2e-4
680.0 (8-2), 5e-4 660.3 (5-5). The descent at the same spot is weaker than
the as-move, 5e-4 664.8 (7-3), and it loses on S/M.

There is one way to read this. **On this track the gain grows with the
instance size — up to about L.** On S the baseline is already at its 175
ceiling, so diverting budget anywhere breaks even at best; on L the ceiling is
out of reach within the budget, so spending more than half of it on structural
repair wins — the same story as with 1000-customer VRP, now on the assignment shape.

Re-measured on 2026-08-18 (Apple Silicon macOS, one machine, 10 s × 6 seeds,
interleaved A/B alternating baseline and candidate on the same seeds), probing
both ends of the band:

- Peak re-confirmed (L, 1000 cells, 10 s): weight 2e-4 + maxRuin 12 =
  **+6.1% (684.8 vs 645.7, 6-0-0)**. maxRuin 6 gives +1.7%, 24 gives +2.1%,
  weight 1e-4 +4.1% — the 2e-4/12 peak found at 20 s / 10 seeds holds at 10 s.
- 1200 cells (28 classes / 47 teachers / 30 rooms, 952 lessons): 0.2/cells =
  +3.3% (3-0).
- **1480 cells (36 classes / 60 teachers / 37 rooms, 1224 lessons): 10 s
  −5.5% (0-2-1), 20 s −4.8% (0-3)** — a 0.2 budget share backfires beyond
  ~1300 cells. Lowering to 5e-5 only reaches +1.4% (2-1).
- M, 520 cells (10 s, 6 seeds): −1.3% (3-2-1), with a tail where one seed
  drops from 348 to 316 (at 20 s it was +2.0% — near the ceiling it only
  takes budget away).

Hence **the descent default stays 0, and the as-move switches on
automatically only in the measured band**: a negative
`ruin_recreate_as_move_weight` (the new default) means 0.2 / slotRoomCount when
slotRoomCount is within [900, 1300], else 0. An explicit 0 disables it, a
positive value is used as is. Outside the band (S/M, 1480 cells) it is off
rather than extrapolated — there is no reason to make a default out of where
it lost. The canonical solver's field of the same name still defaults to 0
(its value range differs; no automatic rule until it is measured separately).
With this, track-1 L moves from a 6-seed average of 645.7 to 684.8 (+6.1%,
6-0-0), while S/M take the same path as before because the rule gives 0.

Why the descent is weak here is easy to guess. An assignment-shape descent looks at every
change/swap of two lessons — a neighbourhood the ordinary move mix already
samples at random, so it looks at the same place at 1000 times the cost. The
as-move's repair, by contrast, re-places the ruined lessons over the whole
grid — a rearrangement ordinary moves cannot reach in one step. An
employee-rostering domain showed the exact opposite (the descent won and the
as-move added nothing on top); there the value range is just 40 employees, so
an exhaustive descent is cheap.

## How to run

### Locally

```sh
# 0) C++ build + test gate
cmake -B build/st -DCMAKE_BUILD_TYPE=Release -DOPTIMARU_ENGINE_TESTS=OFF -DOPTIMARU_USE_CASES=school_timetabling
cmake --build build/st -j
./build/st/school_timetabling/st_test && ./build/st/school_timetabling/st_canonical_test

# 1) Track 1: cross-check gate → instances → C++ vs CP-SAT → rescoring → table (out/RESULTS_track1.md)
cd benchmarks/school-timetabling/cpsat
export OPTIMARU_USE_CASES=school_timetabling   # keep it set: uv rebuilds the extension when it changes
uv sync                                        # the project, with this checkout's optimaru built in place
uv run school-timetabling-compare --sizes S,M,L --seconds 10 \
  --cpp-bin ../../../build/st/school_timetabling/st_quality_bench

# 2) Track 2: full canonical constraints (out/RESULTS_track2.md)
uv run school-timetabling-compare --track2 --sizes S,M,L --seconds 10

# 3) Track 3: partial timetabling, unassigned_lessons_cost relaxed (out/RESULTS_track3.md)
uv run school-timetabling-compare --track3 --sizes S,M,L --seconds 10
```

### CI

[`school-timetabling-bench.yml`](../../.github/workflows/school-timetabling-bench.yml)
runs exactly that order: ccache build → `st_test`/`st_canonical_test` gate →
track 1 (C++/evaluator cross-check gate first) → track 2 → track 3 →
all three tables published in the job summary (+ instance/solution JSON uploaded as
artifacts).

- Triggers: push to `main` / PR (`optimaru/cpp/**`, `optimaru/school_timetabling/**`,
  `tests/school_timetabling/**`,
  `benchmarks/school-timetabling/**`, the workflow itself), and
  `workflow_dispatch` (per-instance budget `seconds`, default 10).
- The runner is ubuntu-latest (4 vCPUs), so absolute values differ from the
  tables above — the gates (tests / cross-check) are deterministic and stay
  equally valid.

## Interpretation — what fits where

- **C++ engine (this repository)**: 12–13M moves/s on a single thread, so
  with budgets of seconds it has the best soft quality at every size, and the
  first feasible solution comes in milliseconds (11 ms even on L). No runtime
  warm-up or dependencies means an instant cold start, which suits
  **interactive services, short budgets, large instances and rescheduling
  loops**. Limit: it is a search, so it gives no optimality proof or bound
  (`FEASIBLE`/`INFEASIBLE` only; INFEASIBLE means hard violations were left, not a proof).
- **CP-SAT**: exact, so it fits **when you need an optimality proof
  (OPTIMAL/bound) or an infeasibility diagnosis, and when the problem is small
  or the budget is minutes or more**. With this benchmark's 10 s budget it is
  competitive up to S (track 2 S swings between even and ahead of the
  engine), but as size grows even the first feasible solution comes late —
  track-1 M after 6.4 s, L not within 10 s; on track 2 M/L it finds hard 0 but
  trails the engine by 22–34% on soft quality — and that with 8 workers
  against the engine's single thread.

In short: short budgets, large problems and embedding in a service — the C++
engine; small settled schedules that need a proof — CP-SAT. That both
implementations score identically is guaranteed continuously in CI by the
cross-check and parity gates.

# Bin packing

Uniform-capacity one-dimensional bin packing, based on the standard
item-to-bin assignment model (Martello & Toth 1990), with optional item colours and conflict pairs, on this repository's C++ engine
(`optimaru/cpp/optimaru/`).

## Usage

```sh
OPTIMARU_USE_CASES=bin_packing uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/bin_packing
```

```python
from optimaru import bin_packing as bp

problem = bp.BinPackingProblem(10, [bp.Item("A", 8), bp.Item("B", 7), bp.Item("C", 3), bp.Item("D", 2)])
print(bp.solve(problem, time_limit_seconds=1.0, random_seed=42).score)   # 0hard/-2soft
print(problem.bins_used(), problem.bin_loads(), problem.assignment())
```

`solve` returns the problem it was given, solved in place; the score is its `score`
(`.hard`, `.soft`). It takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`, `full_assert`, `on_new_best`, a full `config` (`bin_packing.SolverConfig`)
and `constraints`:

```python
from optimaru.bin_packing.constraints import UsedBinsCost, get_constraints, score

bp.constraint_info()                    # the catalog: name, default level / weight, flags
bp.solve(problem, time_limit_seconds=1.0, constraints=[*get_constraints()[:3], UsedBinsCost(weight=2)])
raw = bp.constraint_costs(problem)      # raw per-bin costs, re-priced in pure Python
score(raw), score(raw, [UsedBinsCost(exponent=2.0)])
score(raw, [UsedBinsCost(), UsedBinsCost(weight=100, offset=10)])   # a repeat, shifted
```

The JSON contract is `optimaru.bin_packing.models` (`BinPackingProblemModel.model_validate(...)
.to_core()` / `.from_core(problem)`, with the outputs `score_hard`, `score_soft`,
`bins_used`, `bin_loads`).

## Domain (assignment shape)

- `Item.bin` — the only decision variable: a 0-based index into n identical bins
  (n = the item count, which always suffices), `-1` = unassigned. Every solve returns
  every item assigned; `-1` appears only in unsolved input.
- `Item.weight` (> 0) and the problem's `bin_capacity` (> 0); `Item.color` (optional);
  `conflicts` — item-id pairs that must not share a bin (a pair listed twice, or
  reversed, counts per listing).
- `warm_start` — start the search from the items' bins (see below); every solve clears it.

## Constraints

The score is the engine's `Score` type as it is, with no mapping.

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `bin_capacity_cost` | `BinCapacityCost` | hard | 1 | per bin: `max(0, load - bin_capacity)` |
| `color_conflict_cost` | `ColorConflictCost` | hard | 1 | per bin: every item beyond the first of its colour (`sum over colours of max(0, n_c - 1)`) |
| `item_conflict_cost` | `ItemConflictCost` | hard | 1 | per bin: the listed conflict pairs with both items in it |
| `used_bins_cost` | `UsedBinsCost` (global) | soft | 1 | the number of bins holding at least one item |

Every constraint is flippable (`level="hard"` / `level="soft"`, or `Level.HARD` /
`Level.SOFT`; `level="medium"` raises `ValueError`, the domain has no medium level), none
is a reward and all four are scored by default (`default_enabled`);
`bin_packing.constraint_info()` reports exactly this table from the C++ components
(`kDefaultLevel` / `kDefaultWeight`). Unassigned items contribute nothing.

### Scoring and the Python API

- **No weights on the problem.** `BinPackingProblem` and `BinPackingProblemModel` carry
  no constraint weights (`extra="forbid"`). Weights come only from constraint
  selections; the defaults (1 each) are the components'.
- **`SolverConfig`** has the engine knobs (`spent_limit_seconds`, `step_count_limit`,
  `unimproved_*`, `parallel_solver_count`, `late_acceptance_size`, `ruin_recreate_*`,
  move weights, `value_recommender_probability`, `empty_bin_moves`,
  `empty_bin_candidates`, `repack_moves`, `repack_partner_bins`, `trade_moves`,
  `fill_tie_break`, `best_score_limit`,
  `on_new_best`, `full_assert`) set as attributes (`SolverConfig()` takes no arguments).
  Zero budget is `spent_limit_seconds = 0`; a fixed number of steps is
  `step_count_limit = N`. `on_new_best` is called as `on_new_best(step, score)`.
- **Exponent granularity.** The engine shapes a unit constraint per unit and a global
  on its total. The units are bins: `bin_capacity_cost`, `color_conflict_cost` and
  `item_conflict_cost` are shaped per bin (red x3 + blue x2 in one bin at exponent 2:
  3² = 9; two violated pairs in one bin: 2² = 4), and `used_bins_cost` shapes the count
  (`count ** exponent`).
- **`constraint_costs()` shape.** The standard C++-package dict `{name: {costs, weight,
  exponent, offset, clip, global, level, reward, level_flippable, name, label,
  default_weight, default_level}}`. The three per-bin constraints list one cost per bin
  0..n-1; `used_bins_cost` is `[count]`. Hard costs are rounded per bin, as the engine
  does.
- **Core checks.** Scoring or solving a core problem refuses a `bin_capacity` <= 0, a
  repeated item id, a weight <= 0, a conflict naming an unknown item or the same item
  twice, and a bin outside -1..n-1 (`ValueError`; the pydantic model refuses all but the
  last), in `solve`, `calculate_score` and `constraint_costs` alike. A plain solve
  ignores the input bins.
- **Construction.** Best fit decreasing; an item that fits nowhere when bin n would be
  next goes to the lowest empty bin, so the value range stays 0..n-1. Parallel worker
  i > 0 starts from the same order with equal weights shuffled.
- **Bins are relabelled** 0..k-1 by first appearance when the search moved anything
  (score-neutral), so `bin_loads` has no gaps; a zero-budget seed comes back unchanged.
- **Constraints path.** `solve(..., constraints=...)` is single-start
  (`parallel_solver_count` is ignored there, as in facility-location).
- `problem.items[i]` returns a copy (as in every C++ package): write bins with
  `problem.set_assignment([...])`; `problem.assignment()` reads them. `Score` keeps
  `==`, ordering and hashing; its repr is `<Score 0hard/-2soft>`. `problem.score` is
  read-only (set by `calculate_score` / `solve`); `Item` objects compare by identity.

### Offsets and repeated picks

Every constraint takes the knobs `weight`, `level`, `exponent` and, keyword-only,
`offset` and `clip`, and may be picked several times. A raw cost `x` (per bin for the
three per-bin constraints, the bin count for `used_bins_cost` — the granularity of
`exponent`) is shaped as `sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the
default) or `(x - offset) ** exponent`, sign-preserving (`clip=False`, which lowers the
penalty of every bin 0..n-1 below the offset, an empty one included); `offset=0` is the
plain power shaping. Both apply per bin (or to the bin count), not to the sum over the
bins. `clip=False` with an offset is refused on hard (the three per-bin constraints by
default), because hard 0 must mean no violation. Each pick is its own instance with its
own knobs, its level included, and the instances add up:

```python
from optimaru.bin_packing.constraints import UsedBinsCost, get_constraints

bp.solve(problem, constraints=[
    *get_constraints(),                    # the defaults
    UsedBinsCost(weight=100, offset=10),   # + 100 per bin beyond the tenth
])
```

`bp.constraint_costs(problem, picks)` and `bp.calculate_score(problem, picks)` take the
same picks (the second keyed `used_bins_cost#2`); only `constraints=None` uses the
monolithic calculator. The CP-SAT reference takes integral offsets.

## C++

- `optimaru/bin_packing/cpp/include/bp/domain.hpp` — domain + `optimaru::SolutionTraits` specialization
- `optimaru/bin_packing/cpp/include/bp/constraints/` — one component per constraint: `bin_capacity_cost.hpp`,
  `color_conflict_cost.hpp`, `item_conflict_cost.hpp`, `used_bins_cost.hpp`
  (+ `common.hpp`); `optimaru/bin_packing/cpp/include/bp/constraints.hpp` assembles the default set
  (`defaultConstraints()`)
- `score_calculator` — full recompute (the reference) and `prepare()` (fact checks,
  colour classes, resolved conflict pairs)
- `incremental_calculator` — before/after hooks, O(1) plus the moved item's conflict
  partners per move
- `construction` — best-fit decreasing around a seed, compaction
- `optimaru/bin_packing/cpp/include/bp/moves.hpp` — the domain's macro moves, `EmptyBinMove`, `RepackMove`
  and `TradeMove` (header-only, so the tests drive them directly)
- `optimaru/bin_packing/cpp/include/bp/fill_late_acceptance.hpp` — late acceptance with the rule for a
  step that keeps the score (`fill_tie_break`)
- `solver` — engine wrapper (construction + Late Acceptance + the macro moves + adaptive
  ruin & recreate, parallel starts)
- `optimaru/bin_packing/cpp/src/main.cpp` — demo on the web sample (20 items, colours, conflicts; 8 bins)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPTIMARU_USE_CASES=bin_packing
cmake --build build -j2
ctest --test-dir build --output-on-failure   # bp_test + bp_plugin_test
./build/bin_packing/bp_demo 2   # 2 s solve; checks hard 0 (exit 0)
```

`bp_test`: per-constraint hand checks (a positive and a zero case each, monolithic and
incremental), 5000 random moves incremental == full (the score and each constraint's
raw total after every move, each constraint non-zero during the walk; flat and
hash-map colour counts), the construction cases, zero-budget / warm-start
behaviour, ScoreChecks::Full solve, samples, parallel starts, compaction, fact checks, and
the macro moves: hand cases (`RepackMove` empties a bin `EmptyBinMove` can only empty
into a clash, with and without a partner bin) and a walk of thousands of applied and
undone moves, incremental == full after each apply and each undo, an undo restoring
the assignment exactly, `RepackMove` never adding a hard violation, once plain and once
under a ScoreChecks::Full editor; `TradeMove` by hand (it closes a bin, fills one, trades
between two full bins, finds no trade where there is none) and on the same walk; and
`FillLateAcceptance` (which steps it takes, its copy of the loads after a refusal and
after a change it was not told about, a capacity of 2^40).
`bp_plugin_test`: plugin assembly == monolith (score and per-constraint totals),
plugin incremental == full (every unit cost after every move, for the defaults and
with exponents != 1 and fractional hard weights), partial assemblies, overrides
(including the per-bin exponent), `constraint_totals()`, ScoreChecks::Full plugin solve,
raw costs by hand, the catalog, and the macro-move walk on the plugin path.

## Search

Best-fit decreasing builds the start; late acceptance then works on it with the
engine's change / swap moves, its adaptive ruin-and-recreate kicks, and three macro
moves of the domain (`optimaru/bin_packing/cpp/include/bp/moves.hpp`). They exist because
`used_bins_cost` is a step function: a bin only stops costing when its last item
leaves, which no single change can show the acceptor.

- **`EmptyBinMove`** (`empty_bin_moves`) moves every item of one of the
  `empty_bin_candidates` lightest bins into the best-fitting other bin with room, in
  one move. It checks the capacity only.
- **`RepackMove`** (`repack_moves`, `repack_partner_bins`) takes the items out of one
  of those light bins *and* out of 1 to `repack_partner_bins` (2) other light bins, and
  puts them back heaviest first into the fullest bin where each adds no capacity, colour
  or conflict violation; one that fits no other bin goes to the emptied light bin
  (or, if it clashes there too, a new bin). So the move either saves a bin or
  reshuffles the light bins at an equal score. It adds no hard violation as long as
  every item fits the capacity on its own and some bin is free.
- **`TradeMove`** (`trade_moves`) trades none, one or two items of one bin for none,
  one or two of another. Of the trades with up to 16 other bins that keep both bins
  within the capacity and leave the fuller of the two no less full, it makes the one
  that raises the two bins' sum of squared loads most; when no trade raises it, one
  after which both hold what they held, in other items. That second kind is how an item gets out of a full bin and to the bin
  that needs it. It checks the capacity only.

**A step that keeps the score** (`fill_tie_break`). Every packing with the same number
of bins has the same score, and plain late acceptance takes every step from one to the
next, so the change and swap moves undo what `TradeMove` gains. With `fill_tie_break`
(the default) such a step is taken when it does not lower the *fill*, the sum of the
squared bin loads, and otherwise one time in eight
(`optimaru/bin_packing/cpp/include/bp/fill_late_acceptance.hpp`). The fill is not part
of the score: the score, `best_score_limit`, the unimproved limits and `on_new_best` see
the number of bins alone. A step that changes the score is judged as before.

What the two add, on Falkenauer's instances
([`benchmarks/bin-packing`](../../../benchmarks/bin-packing/README.md): three instances
of each of the eight classes, two seeds, 10 s, one thread), as runs that end above the
capacity bound ceil(total weight / capacity), which is the best known bin count of all 24:

| `trade_moves` | `fill_tie_break` | runs above the bound, of 48 | where |
| --- | --- | ---: | --- |
| on | on | 0 | |
| on | off | 7 | the triplets of 249 items (1 of 6 runs) and of 501 (6 of 6) |
| off | on | 40 | every class |
| off | off | 39 | every class; this is the search of 0.5.0 |

On instances with colours or conflicts `EmptyBinMove` alone leaves the search one to
three bins above the bound: emptying a bin into the others nearly always breaks a colour
or a conflict and late acceptance turns the move down. `RepackMove` and `TradeMove` each
close those bins. Seeds 1-3, one thread, 2 s (at 10 s one run differs: g11 with all three
off, 44 bins), 120 items of weight 10..60 on capacity 100, about 40% in one of 4 colours,
10 conflicts
([`benchmarks/bin-packing/bench/quality.py`](../../../benchmarks/bin-packing/bench/quality.py)),
bins used:

| instance (weight lower bound) | `repack_moves` off | `trade_moves` and `fill_tie_break` off | all three off | default |
| --- | --- | --- | --- | --- |
| sq1 (43) | 43, 43, 43 | 43, 43, 43 | 45, 45, 45 | 43, 43, 43 |
| g11 (42) | 42, 42, 42 | 42, 42, 42 | 45, 45, 45 | 42, 42, 42 |
| g22 (44) | 44, 44, 44 | 44, 44, 44 | 45, 45, 46 | 44, 44, 44 |
| g33 (42) | 42, 42, 42 | 42, 42, 42 | 44, 44, 43 | 42, 42, 42 |

The full table is in [`benchmarks/bin-packing`](../../../benchmarks/bin-packing/README.md);
`RepackMove` is what gets its 120-item triplet instance from 41 bins to 40.

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of a
`BinPackingProblemModel` whose search starts from `solution`, a solved model or
the same thing as a JSON dict. Items are matched by id; items left out or at
bin -1 are placed around the seeded bins by the best-fit construction before the
search starts, and a complete seed skips construction (with `spent_limit_seconds = 0`
it comes back exactly, gaps included). Unknown ids, duplicates and bins outside
the item count raise `ValueError`. A seed is used once: the solve clears
`warm_start`, and a plain solve ignores input bins.

```python
from optimaru.bin_packing import with_initial_solution
from optimaru.bin_packing.models import BinPackingProblemModel

solved = BinPackingProblemModel.from_core(problem)
warm = with_initial_solution(solved, solved.model_dump(mode="json")).to_core()
print(bp.solve(warm, time_limit_seconds=1.0, random_seed=42).score, warm.bin_loads())
```

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **assignment** shape; the domain solver that configures the
engine is `optimaru/bin_packing/cpp/include/bp/solver.hpp` + `optimaru/bin_packing/cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves (weights in
`Solver::Config`; `value_recommender_probability` biases change moves toward bins in
use). `EmptyBinMove` (moves every item of a light bin into the other bins in one step),
`RepackMove` (re-places the items of a few light bins without a clash) and `TradeMove`
(trades items between two bins so that one gets fuller) are defined
in `optimaru/bin_packing/cpp/include/bp/moves.hpp` and registered in `optimaru/bin_packing/cpp/src/solver.cpp` (`runEngine`) —
use them as the template, and give a new move a walk in `bp_test` (j). A new move is an `optimaru::MoveOperator<…>` registered with
`engine.add_move_operator(...)` next to the engine construction in `optimaru/bin_packing/cpp/src/solver.cpp`;
a ruin for LNS is a `RuinOperator` registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** The domain places every item before the
engine runs (`optimaru/bin_packing/cpp/src/construction.cpp`: best fit over the items by decreasing
weight, around the seeded bins); the engine only fills what is still unassigned, so a
different heuristic is a different `constructBins` order or rule.

**Adding a hard or soft cost.** [Adding a constraint, step by step](extending.md) takes one new
constraint through this use case's files in six steps: changed from Python, written as a
component, tested, made cheaper per move, moved to the default fast path and measured.

## Benchmark

[`benchmarks/bin-packing`](../../../benchmarks/bin-packing) compares this use case with
OR-Tools CP-SAT on Falkenauer's public uniform and triplet instances (`bench/run_bench.py`) and on generated
ones (`python -m bin_packing_cpsat.benchmark`).

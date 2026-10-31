# Bin packing

Uniform-capacity one-dimensional bin packing, based on the compact model used
in the [Hexaly BPP template](https://www.hexaly.com/templates/bin-packing-problem-bpp),
with optional item colours and conflict pairs, on this repository's C++ engine
(`include/optimaru/`).

## Domain (BASIC variable)

- `Item.bin` — the only planning variable: a 0-based index into n identical bins
  (n = the item count, which always suffices), `-1` = unassigned. Every solve returns
  every item assigned; `-1` appears only in unsolved input.
- `Item.weight` (> 0) and the problem's `bin_capacity` (> 0); `Item.color` (optional);
  `conflicts` — item-id pairs that must not share a bin (a pair listed twice, or
  reversed, counts per listing).
- `warm_start` — start the search from the items' bins (see below); every solve clears it.

## Constraints (HardSoftScore — the engine score type as-is, no mapping)

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `bin_capacity_cost` | `BinCapacityCost` | hard | 1 | per bin: `max(0, load - bin_capacity)` |
| `color_conflict_cost` | `ColorConflictCost` | hard | 1 | per bin: every item beyond the first of its colour (`sum over colours of max(0, n_c - 1)`) |
| `item_conflict_cost` | `ItemConflictCost` | hard | 1 | per bin: the listed conflict pairs with both items in it |
| `used_bins_cost` | `UsedBinsCost` (global) | soft | 1 | the number of bins holding at least one item |

Every constraint is flippable (`hard=True` / `hard=False`), none is a reward and all
four are scored by default (`default_enabled`); `bin_packing.constraint_info()`
reports exactly this table from the C++ components (`kDefaultLevel` /
`kDefaultWeight`). Unassigned items contribute nothing.

### 0.2.0

- **Names.** `bin_capacity` -> `bin_capacity_cost`, `color_compatibility` ->
  `color_conflict_cost`, `item_conflicts` -> `item_conflict_cost`, `minimize_bins` ->
  `used_bins_cost`; the spec classes `BinCapacity` / `ColorCompatibility` /
  `ItemConflicts` / `MinimizeBins` -> `BinCapacityCost` / `ColorConflictCost` /
  `ItemConflictCost` / `UsedBinsCost`, and the module-level default instances follow
  the snake names (all four are in `constraints.__all__`). No aliases. The problem
  field `bin_capacity` keeps its name.
- **No weights on the problem.** `capacity_weight`, `used_bin_weight`, `color_weight`
  and `conflict_weight` are gone from `BinPackingProblem` and `BinPackingProblemModel`
  (`extra="forbid"` refuses them). Weights come only from constraint selections; the
  defaults (1 each) are the components'.
- **C++ engine.** The pure-Python simulated annealing is replaced by the engine (late
  acceptance over change / swap moves, the domain's `EmptyBinMove`, adaptive
  ruin-and-recreate). `SolverConfig` has the engine knobs (`spent_limit_seconds`,
  `step_count_limit`, `unimproved_*`, `parallel_solver_count`, `late_acceptance_size`,
  `ruin_recreate_*`, move weights, `value_recommender_probability`,
  `empty_bin_moves`, `empty_bin_candidates`, `best_score_limit`, `on_new_best`,
  `full_assert`) set as attributes (`SolverConfig()` takes no arguments);
  `max_iterations`, `stagnation_limit`, `temperature` and `cooling_rate` are gone.
  Zero budget is `spent_limit_seconds = 0` (was `max_iterations = 0`); a fixed
  number of steps is `step_count_limit = N` (was `max_iterations = N`).
  `on_new_best` is called as `on_new_best(step, score)` (was `on_new_best(problem)`).
  Seeds do not reproduce 0.1.x results.
- **Exponent granularity.** The engine shapes a unit constraint per unit and a global
  on its total. The units are bins: `color_conflict_cost` is shaped per bin (0.1.x per
  (bin, colour)) and `item_conflict_cost` per bin (0.1.x per pair, a 0/1 entry). Totals
  and every exponent-1 score are unchanged; with an exponent != 1 the score differs when
  one bin holds excess in two colours or two violated pairs (red x3 + blue x2 in one bin
  at exponent 2: 3² = 9, was 2² + 1² = 5; two violated pairs in one bin: 4, was 2).
  `used_bins_cost` still shapes the count (`count ** exponent`), and
  `bin_capacity_cost` was already per bin.
- **`constraint_costs()` shape.** The standard C++-package dict `{name: {costs, weight,
  exponent, hard, global, level, reward, level_flippable, name, label}}`. The three
  per-bin constraints list one cost per bin 0..n-1 (0.1.x: capacity per bin 0..max bin,
  colour per present (bin, colour), conflicts per pair); `used_bins_cost` stays `[count]`.
  Hard costs are rounded per bin, as the engine does (differs from 0.1.x only for
  fractional hard weights).
- **Stricter core checks.** Scoring or solving a core problem refuses a repeated item
  id, a weight <= 0, a conflict naming an unknown item or the same item twice, and a bin
  outside -1..n-1 (`ValueError`; the pydantic model already refused all but the last).
  A plain solve still ignores the input bins.
- **Construction.** The same best fit decreasing as 0.1.x (`_first_fit`), except that
  an item that fits nowhere when bin n would be next goes to the lowest empty bin (0.1.x
  returned bin n, which `with_initial_solution` then refused). Parallel worker i > 0
  starts from the same order with equal weights shuffled (0.1.x: a full shuffle).
- **Bins are relabelled** 0..k-1 by first appearance when the search moved anything
  (score-neutral), so `bin_loads` has no gaps; a zero-budget seed comes back unchanged.
- **Constraints path.** `solve(..., constraints=...)` is single-start
  (`parallel_solver_count` is ignored there, as in facility-location).
- `problem.items[i]` returns a copy (as in every C++ package): write bins with
  `problem.set_assignment([...])`; `problem.assignment()` reads them. `Score` keeps
  `==`, ordering and hashing; its repr is `<Score 0hard/-2soft>`.

## cpp/

- `include/bp/domain.hpp` — domain + `optimaru::SolutionDescriptor` specialization
- `include/bp/constraints/` — one component per constraint: `bin_capacity_cost.hpp`,
  `color_conflict_cost.hpp`, `item_conflict_cost.hpp`, `used_bins_cost.hpp`
  (+ `common.hpp`); `include/bp/constraints.hpp` assembles the default set
  (`defaultConstraints()`)
- `score_calculator` — full recompute (the reference) and `prepare()` (fact checks,
  colour classes, resolved conflict pairs)
- `incremental_calculator` — before/after hooks, O(1) plus the moved item's conflict
  partners per move
- `construction` — best-fit decreasing around a seed, compaction
- `solver` — engine wrapper (construction + Late Acceptance + `EmptyBinMove` + adaptive
  ruin & recreate, parallel starts)
- `src/main.cpp` — demo on the web sample (20 items, colours, conflicts; 8 bins)

```bash
cmake -S use-cases/bin-packing/cpp -B build/bin-packing -DCMAKE_BUILD_TYPE=Release
cmake --build build/bin-packing -j2
ctest --test-dir build/bin-packing --output-on-failure   # bp_test + bp_plugin_test
./build/bin-packing/bp_demo 2   # 2 s solve; checks hard 0 (exit 0)
```

`bp_test`: per-constraint hand checks (a positive and a zero case each, monolithic and
incremental), 5000 random moves incremental == full, the construction against 0.1.x,
zero-budget / warm-start behaviour, FULL_ASSERT solve, samples, parallel starts,
compaction, fact checks. `bp_plugin_test`: plugin assembly == monolith, plugin
incremental == full, partial assemblies, overrides (including the per-bin exponent),
`constraintTotals()`, FULL_ASSERT plugin solve, raw costs by hand, the catalog.

## Usage

```sh
cd use-cases/bin-packing/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import bin_packing as bp

problem = bp.BinPackingProblem(10, [bp.Item("A", 8), bp.Item("B", 7), bp.Item("C", 3), bp.Item("D", 2)])
print(bp.solve(problem, time_limit_seconds=1.0, random_seed=42))   # 0hard/-2soft
print(problem.bins_used(), problem.bin_loads(), problem.assignment())
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`,
`stop_at_first_feasible`, `on_new_best`, a full `config` (`bin_packing.SolverConfig`)
and `constraints`:

```python
from bin_packing.constraints import UsedBinsCost, get_constraints, score

bp.constraint_info()                    # the catalog: name, default level / weight, flags
bp.solve(problem, constraints=[*get_constraints()[:3], UsedBinsCost(weight=2)])
raw = bp.constraint_costs(problem)      # raw per-bin costs, re-priced in pure Python
score(raw), score(raw, [UsedBinsCost(exponent=2.0)])
```

The JSON contract is `bin_packing.models` (`BinPackingProblemModel.model_validate(...)
.to_core()` / `.from_core(problem)`, with the outputs `score_hard`, `score_soft`,
`bins_used`, `bin_loads`).

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
from bin_packing import with_initial_solution
from bin_packing.models import BinPackingProblemModel

solved = BinPackingProblemModel.from_core(problem)
warm = with_initial_solution(solved, solved.model_dump(mode="json")).to_core()
print(bp.solve(warm, time_limit_seconds=1.0, random_seed=42), warm.bin_loads())
```

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/bp/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves (weights in
`Solver::Config`; `value_recommender_probability` biases change moves toward bins in
use). `EmptyBinMove` (moves every item of a light bin into the other bins in one step)
is defined and registered in `cpp/src/solver.cpp` (`runEngine`) — use it as the
template. A new move is an `optimaru::MoveOperator<…>` registered with
`engine.addMoveOperator(...)` next to the engine construction in `cpp/src/solver.cpp`;
a ruin for LNS is a `RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** The domain places every item before the
engine runs (`cpp/src/construction.cpp`: best fit over the items by decreasing
weight, around the seeded bins); the engine only fills what is still unassigned, so a
different heuristic is a different `constructBins` order or rule.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/bp/constraints/` (currently `bin_capacity_cost`, `color_conflict_cost`,
`item_conflict_cost`, `used_bins_cost`):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint`
   per bin or a `GlobalConstraint`; positive raw cost = penalty, 0 = no violation)
   declaring `static constexpr double kDefaultWeight` and
   `static constexpr optimaru::Level kDefaultLevel`, an
   `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `cpp/include/bp/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to `score_calculator` /
   `incremental_calculator`, weighted by the component's `kDefaultWeight` —
   `bp_plugin_test` checks they agree bit for bit — and a positive and a zero
   hand-calculated case to `bp_test` / `bp_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `python/bin_packing/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRuleCost(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/bin-packing/cpsat`](../../benchmarks/bin-packing/cpsat) (a plugin
   subclassing the spec; defaults come from `constraint_info()`) and extend its
   pinned-assignment parity test, so both solvers keep one semantics.

## Benchmark

[`benchmarks/bin-packing`](../../benchmarks/bin-packing) compares this use case with
OR-Tools CP-SAT on Hexaly's public instances (`bench/run_bench.py`) and on generated
ones (`python -m bin_packing_cpsat.benchmark`).

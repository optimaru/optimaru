# Max-Cut

Weighted Max-Cut with fixed vertex sides, same-side and different-side pairs,
side-size bounds and an optional balance term, on this repository's C++ engine
(`include/optimaru/`): split the vertices into two sides so that the total
weight of the edges between the sides is as large as possible while every side
rule holds. The base formulation is the standard weighted Max-Cut problem.
[`benchmarks/max-cut/cpsat`](../../benchmarks/max-cut/cpsat) holds the exact
CP-SAT reference.

## Domain (assignment shape)

- The **side** of each vertex — the only decision variable: `0` or `1` (the value
  range is `{0, 1}`, so the engine's change move is a flip and its swap move
  exchanges a side-0 and a side-1 vertex). On the model it is `partition`
  (`{vertex: side}`); on the engine object `assignment()` / `set_assignment()`
  (one entry per vertex, `-1` = unassigned).
- A vertex in `fixed_partition` is **pinned**: the engine never moves, swaps or
  ruins it.
- Facts: `vertices` (unique ids), weighted undirected `edges` (weight >= 0; a
  self-loop is never cut, parallel edges add up), `fixed_partition`,
  `same_partition_pairs`, `different_partition_pairs` (a pair may repeat — each
  entry counts — and may name one vertex twice: a same self-pair is never split,
  a different self-pair is always joined), `min_partition_size` /
  `max_partition_size` (inclusive bounds on EACH side; None = 0 / |V|).
- `warm_start` — start from `partition`; vertices it leaves out take the default
  start. A plain solve ignores the input partition. Every solve resets it.
- Start partition: vertex `i` on side `i % 2`, a fixed vertex
  on its fixed side, a warm-start partition over both.
- **Exact size** (`min_partition_size == max_partition_size`, both given): the
  search only swaps sides (change moves, ruin-and-recreate and descent are
  forced off, whatever `SolverConfig` says), so the side sizes of the start
  partition are kept — a seed of the wrong size keeps that size and its hard
  penalty. This holds for an unseeded solve too: when fixed vertices unbalance
  the `i % 2` start, the result stays infeasible even if a feasible partition
  exists; seed a partition of the right size with
  `with_initial_solution` instead. With fewer than two free vertices there is no
  search.
- An unassigned vertex (`-1`, transient in the engine's ruin-and-recreate or set
  by a C++ caller) is in no side: it adds nothing to the cut, the sizes, the
  imbalance, `fixed_partition_cost` or `different_partition_pairs_cost`, and a
  same-side pair is split while its first vertex is assigned and its second is
  not. `calculate_score` / `constraint_costs` refuse a partial partition
  (`ValueError`); `solve` fills it in.

## Constraints (Score — the engine score type as-is, no mapping)

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `fixed_partition_cost` | `FixedPartitionCost` | hard | 1 | per side: fixed vertices on that side that belong on the other one |
| `same_partition_pairs_cost` | `SamePartitionPairsCost` | hard | 1 | per side: same-side pairs split, each charged to its first vertex's side |
| `different_partition_pairs_cost` | `DifferentPartitionPairsCost` | hard | 1 | per side: different-side pairs with both vertices on that side |
| `partition_size_bounds_cost` | `PartitionSizeBoundsCost` | hard | 1 | per side: `max(0, min - n) + max(0, n - max)`, `n` = the side's size |
| `partition_imbalance_cost` | `PartitionImbalanceCost` | soft | 0 (opt-in) | one total: `abs(n0 - n1)` |
| `cut_weight_cost` | `CutWeightCost` | soft | 1 | **reward**, one total: `-(cut weight)`, always <= 0, so the soft score is `+cut weight` (0 = nothing cut, the worst value) |

The first four are unit constraints (`constraint_costs()` reports
`[side 0, side 1]`), the last two globals (one total). Five are scored by
default; `partition_imbalance_cost` has default weight 0 (`default_enabled` is
False): give it a weight to trade cut weight for balance. Every constraint but
the reward is flippable (`level="hard"` / `level="soft"`, or `Level.HARD` /
`Level.SOFT`; `level="medium"` raises `ValueError`, the domain has no medium level).
`cut_weight_cost` is a reward: it is always soft — `level="hard"` raises `ValueError`
in `solve`, `calculate_score`, `constraints.score()` and the CP-SAT reference — and
its level cannot change. `max_cut.constraint_info()` reports exactly this table from
the C++ components (`kDefaultLevel` / `kDefaultWeight`).

### Knobs: exponent, offset, clip, repeats

Every constraint takes `weight`, `level` and `exponent`, plus `offset` and `clip`
(keyword-only), and may be picked several times. The four unit constraints shape each
side's raw cost `x`, the imbalance and the cut their single total:
`sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which charges both sides, a side
of raw 0 included, and rewards a side below the offset); `offset=0` is the plain shaping.
Both apply per side (or to the one total), not to the sum over the sides, so three fixed
vertices off their side on side 0 cost `3 ** 2 = 9` at `exponent=2`. Hard contributions
are rounded per side (size bounds `[1, 1]` at weight 0.4: `0`). `clip=False` with an
offset is refused on hard (the four unit constraints by default), because hard 0 must mean
no violation: a clean side would otherwise earn a hard reward that cancels the other side's
violation. On the `cut_weight_cost` reward (raw `-cut`) a threshold offset rewards only the
cut beyond it and a literal one adds `offset` to the reward. Each pick is its own instance
with its own knobs, its level included, and the instances add up:

```python
max_cut.solve(problem, constraints=[
    *get_constraints()[:4],
    PartitionImbalanceCost(weight=1),                          # |n0 - n1|
    PartitionImbalanceCost(weight=5, exponent=2, offset=2),    # + 5 x (imbalance beyond 2)^2
    CutWeightCost(),
])
```

`max_cut.constraint_costs(problem, picks)` returns
`{name: {"costs", "weight", "exponent", "offset", "clip", "global", "level", "reward",
"level_flippable", "name", "label", ...}}`, with `[side 0, side 1]` for the four unit
constraints and one total for the two globals; it keys the second pick
`partition_imbalance_cost#2` and reports every entry's `offset`, `clip` and the `level` in
effect. `calculate_score` and `constraint_costs` raise `ValueError` on a partial
partition (`solve` fills it in), and the model refuses `partition` entries for unknown
vertices and sides other than 0 / 1. The CP-SAT reference takes the same picks (integral
offsets; it rejects an exponent other than 1).

### Python API

- **`SolverConfig`** is the engine's (the other C++ packages' knobs:
  `spent_limit_seconds`, `step_count_limit`, `unimproved_*`, `random_seed`,
  `parallel_solver_count`, `late_acceptance_size`, `full_assert`, `ruin_recreate*`,
  `change/swap_move_weight`, `descent_weight`, `best_score_limit`, `on_new_best`),
  set by attribute. Zero budget = `spent_limit_seconds = 0` (`step_count_limit = 0`
  means "no limit"); a bounded search = `step_count_limit = N`. Without a time limit a
  solve runs the 30 s budget. `on_new_best(step, score)`. Multi-start runs independent
  engine searches from the same start.
- `solve()` returns the engine `Score` (`.hard`, `.soft`, `is_feasible()`) and takes the
  engine object or the model (written back in place). `calculate_score(problem,
  constraints)` returns the engine `Score` for the defaults and a
  `constraints.Score(hard, soft)` named tuple for a selection; `constraints.score()`
  re-prices raw costs. With `constraints=...` a solve is single-start
  (`parallel_solver_count` is ignored), as in every C++ package.
- `MaxCutProblemModel.to_core()` / `from_core()` convert to and from the real engine
  object; `from_core` reports `cut_weight` and the score once the core was scored, and
  lists `fixed_partition` in vertex order.

## cpp/

- `include/mc/domain.hpp` — domain + `optimaru::SolutionTraits` specialization
  (with `is_pinned` for fixed vertices) and the `-1` rules
- `include/mc/constraints/` — one component per constraint:
  `fixed_partition_cost.hpp`, `same_partition_pairs_cost.hpp`,
  `different_partition_pairs_cost.hpp`, `partition_size_bounds_cost.hpp`,
  `partition_imbalance_cost.hpp`, `cut_weight_cost.hpp` (+ `common.hpp`: the
  aliases, the prepare guard and the member-change base of the globals);
  `include/mc/constraints.hpp` assembles the default set (`defaultConstraints()`,
  also the catalog order)
- `score_calculator` — `prepare()` (validation + CSR adjacency / pair incidence)
  and the full recompute (the reference, `rawCosts()`)
- `incremental_calculator` — O(degree + pair degree of the vertex) per move
- `solver` — engine wrapper (the alternating start partition + Late Acceptance over flip /
  swap moves + adaptive ruin & recreate; swaps only under exact size bounds;
  independent parallel starts on worker threads, an exception in one of them is
  rethrown by `solve()`)
- `src/main.cpp` — the web demo solved and explained per constraint

```bash
cmake -S use-cases/max-cut/cpp -B build/max-cut -DCMAKE_BUILD_TYPE=Release
cmake --build build/max-cut -j4
ctest --test-dir build/max-cut --output-on-failure --no-tests=error
./build/max-cut/mc_demo 2
```

`mc_test`: per-constraint hand checks (positive and zero) on the full recompute and
the incremental calculator, the edge cases (self-loops, parallel and zero-weight
edges, self-pairs, one-sided bounds, the `-1` rules), incremental == full recompute
over random flip / swap / unassign / batch moves (the score and every constraint's
raw total after each move, each constraint non-zero during the walk), ScoreChecks::Full
solves (ALNS kicks and exact size), the exact-size contract, pinning under
multi-start, a worker exception rethrown instead of terminating the process, the
zero budget, an empty graph, brute-force optima, determinism and validation.
`mc_plugin_test`: default assembly == monolith bit for bit (score and unit costs),
plugin incremental == full recompute per constraint (with the opt-in imbalance
switched on too), partial assemblies, overrides (exponent per side, per-side hard
rounding), `constraint_totals()`, ScoreChecks::Full plugin solves, the builder, raw costs
by hand, the catalog and the reward guards, and repeated / shifted components (a
hand-calculated repeat, threshold and literal offsets by hand per side and on the
totals, the literal formula refused on hard at reset and kept on soft, incremental == a
fresh recompute over a random walk, a ScoreChecks::Full solve).

## Usage

```sh
cd use-cases/max-cut/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import max_cut
from max_cut.models import EdgeModel, MaxCutProblemModel

problem = MaxCutProblemModel(
    vertices=["A", "B", "C"],
    edges=[EdgeModel(source="A", target="B", weight=2), EdgeModel(source="B", target="C", weight=3),
           EdgeModel(source="A", target="C", weight=4)],
)
print(max_cut.solve(problem, time_limit_seconds=1.0, random_seed=42))   # 0hard/7soft; the model, in place
print(problem.partition, problem.cut_weight)

core = problem.to_core()                                                # or the engine object
max_cut.solve(core, time_limit_seconds=1.0, random_seed=42)
print(MaxCutProblemModel.from_core(core).cut_weight)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`,
`stop_at_first_feasible`, `on_new_best`, a full `config` (`max_cut.SolverConfig`)
and `constraints`:

```python
from max_cut.constraints import CutWeightCost, PartitionImbalanceCost, PartitionSizeBoundsCost, get_constraints, score

max_cut.constraint_info()     # [ConstraintInfo(name='fixed_partition_cost', default_level='hard', ...), ...]
max_cut.solve(
    problem,
    time_limit_seconds=1.0,
    constraints=[*get_constraints()[:4], PartitionImbalanceCost(weight=2), CutWeightCost()],
)
raw = max_cut.constraint_costs(problem)             # raw costs, re-priced in pure Python:
score(raw), score(raw, [PartitionSizeBoundsCost(level="soft"), CutWeightCost()])
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping (clip=True: only the cut beyond 5 earns the second pick's bonus).
score(raw, [CutWeightCost(), CutWeightCost(weight=0.5, offset=5)])
```

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose
search starts from `solution`, a solved model or the same thing as a JSON dict.
Vertices the partition leaves out take the default start. Any result of `solve`
is accepted, feasible or not. Under exact size bounds the search only swaps
sides, so a seed of the wrong size keeps that size and its hard penalty. Unknown
vertices, sides other than 0 or 1 and sides that contradict `fixed_partition`
raise `ValueError`. With a zero budget (`spent_limit_seconds = 0`) the seed comes
back unchanged.

```python
from max_cut import with_initial_solution

warm = with_initial_solution(problem, problem.model_dump(mode="json"))
max_cut.solve(warm, time_limit_seconds=1.0, random_seed=42)
print(warm.partition, warm.cut_weight)
```

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-use-case).
The solution has the **assignment** shape; the domain solver that configures the engine is
`cpp/include/mc/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes flip (change) and swap moves (weights in
`Solver::Config`) and runs ruin-and-recreate on stagnation (and, with
`ruinRecreateAsMoveWeight > 0`, as an ordinary move — it moves a cluster of vertices
chained by same-side pairs at once). A new move (a gain-guided Kernighan–Lin flip,
say) is an `optimaru::MoveOperator<…>` registered with `engine.add_move_operator(...)`
on every engine `cpp/src/solver.cpp` builds: the single start, each parallel worker
in `searchInParallel()` and the constraint-assembly overload. Under exact size
bounds a move must keep the side sizes.

**Changing the construction heuristic.** `construct()` in `cpp/src/solver.cpp` gives
every vertex a side before the engine starts (the alternating start); the
Python reference-parity tests pin it.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/mc/constraints/`:
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` — an
   `McUnitConstraint` when the cost decomposes per side (`cost(problem, side,
   members)`, plus `delta_cost` when it is a sum of per-member / per-pair terms) or a
   `detail::MemberChangeConstraint` (a global told every join / leave) when it does
   not; positive raw cost = penalty, 0 = no violation — declaring
   `static constexpr double kDefaultWeight` and
   `static constexpr optimaru::Level kDefaultLevel`, an
   `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `cpp/include/mc/constraints.hpp` and add it to `defaultConstraints()` if
   it belongs to the default set (that is also the catalog order);
3. for a default constraint, add the same raw cost to `score_calculator` /
   `incremental_calculator`, weighted by the component's `kDefaultWeight` —
   `mc_plugin_test` checks they agree bit for bit — and a positive and a zero
   hand-calculated case to `mc_test` / `mc_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `python/max_cut/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=[..., MyRuleCost(weight=5, level="hard")]`;
5. mirror it in the CP-SAT model, `external_solver.max_cut` in
   [external-solver](https://github.com/optimaru/external-solver) (a plugin subclassing the
   spec; defaults come from `constraint_info()`), with an exact encoding so its
   `tests/max_cut/test_constraint_parity.py` can pin partitions;
   [`benchmarks/max-cut/cpsat`](../../benchmarks/max-cut/cpsat) runs it against this engine.

## Benchmark

[`benchmarks/max-cut`](../../benchmarks/max-cut) compares this use case with OR-Tools
CP-SAT on the same instances, rescored by the same evaluator.

Latest run (30 s per solver, 3 instances): best score by the engine on 1, CP-SAT on 3
(ties count for each). Full table: [benchmarks/max-cut/README.md](../../benchmarks/max-cut/README.md#results).

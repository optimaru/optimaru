# Max-cut: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its six constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | one C++ test case, the catalog in the tests, the Python reference scorer |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the two monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `uncutEdgesCost`, the number of edges a side keeps inside, whatever
their weight. The cut weight values the heavy edges; this constraint counts every edge that is left
uncut the same. Every snippet below was compiled and run, and every figure measured, on optimaru
0.4.3; a later change to the solver moves the figures, not the steps.

## How a partition is scored

A vertex is on side 0 or on side 1. The score is kept per **unit**: one side, with
the vertices on it as its **members**. A move flips one vertex or swaps two, so the
sides they leave and join are scored again. Four of the six constraints are a sum of one
term per side. The other two, the imbalance and the cut, are one number for the
whole partition: they are **global** components that keep their own totals.

There are two scoring paths, and they give the same score for the same partition
(`mc_plugin_test` checks it on 500 random partitions):

| | `max_cut.solve(problem)` | `max_cut.solve(problem, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/mc/constraints/` |
| Constraints | the six, at their defaults (`partition_imbalance_cost` at weight 0) | the ones you list, with your knobs |
| Steps per second | 6.8 million | 4.1 million |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are g400 of the benchmark (400 vertices, 2,392 edges), 10 million steps,
one thread of an Apple M4 Max; [step 6](#6-measure) has the script. Steps 1 to 4
work on the assembly. Step 5 moves a constraint to the monolithic calculators.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import random

from optimaru import max_cut
from optimaru.max_cut.constraints import CutWeightCost, PartitionImbalanceCost, get_constraints
from optimaru.max_cut.models import EdgeModel, MaxCutProblemModel

rng = random.Random(5)  # g400 of the benchmark
vertices = [f"V{v}" for v in range(400)]
edges = [
    EdgeModel(source=vertices[a], target=vertices[b], weight=rng.randint(1, 10))
    for a in range(400)
    for b in range(a + 1, 400)
    if rng.random() < 0.03
]
problem = MaxCutProblemModel(vertices=vertices, edges=edges)

changed = {"cut_weight_cost", "partition_imbalance_cost", "partition_size_bounds_cost"}
picks = [
    *[rule for rule in get_constraints() if rule.name not in changed],  # the others as they are
    CutWeightCost(weight=2),                                           # heavier (default 1)
    PartitionImbalanceCost(weight=1, level="hard"),                    # soft and off by default
]                                                                      # partition_size_bounds_cost: dropped
result = max_cut.solve(problem, time_limit_seconds=10, constraints=picks)
```

To try weights without solving again, score a finished partition with other picks:

```python
from optimaru.max_cut.constraints import score

raw = max_cut.constraint_costs(result, picks)  # raw cost of every rule, per side or as one total
print(score(raw, picks))                       # the score those picks give this partition
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/max_cut/constraints.py).
Stop here if the constraint you need is one of the six.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

[`constraints/common.hpp`](../../../optimaru/max_cut/cpp/include/mc/constraints/common.hpp)
names two bases. Pick the base by what the cost is:

| The cost is | Base | You write |
| --- | --- | --- |
| a sum of one term per side | `McUnitConstraint` | `cost(problem, side, members)` |
| one number for the whole partition | `detail::MemberChangeConstraint` | `reset()`, `on_member_changed()` and `cost()` |

The example counts the edges inside one side, so it is one function. Four edits:

**1. The component**, `optimaru/max_cut/cpp/include/mc/constraints/uncut_edges_cost.hpp`:

```cpp
#ifndef MC_CONSTRAINTS_UNCUT_EDGES_COST_HPP
#define MC_CONSTRAINTS_UNCUT_EDGES_COST_HPP

#include <vector>

#include "mc/constraints/common.hpp"

namespace mc {

// Soft, off by default (weight 0): uncutEdgesCost — the edges a side keeps
// inside, counted whatever their weight. Raw cost per side u = the edges
// between two different vertices that are both on side u, so 0 = every edge
// is cut. The two sides sum to the count of uncut edges. A self-loop never
// counts (no partition cuts it); parallel edges count once each; an edge with
// an unassigned endpoint is inside no side.
struct UncutEdgesCost final : McUnitConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit UncutEdgesCost(double weight = kDefaultWeight)
        : McUnitConstraint("uncutEdgesCost", weight, kDefaultLevel) {}

    double cost(const MaxCutProblem& problem, int /*unit*/,
                const std::vector<int>& members) const override {
        const std::vector<char> in = detail::memberFlags(problem, members);
        double total = 0.0;
        for (const Edge& edge : problem.edges) {
            if (edge.source != edge.target && in[edge.source] && in[edge.target]) total += 1.0;
        }
        return total;
    }
};

}  // namespace mc

#endif  // MC_CONSTRAINTS_UNCUT_EDGES_COST_HPP
```

The name passed to the base, `"uncutEdgesCost"`, is the struct name with a
lower-case first letter. Python derives `uncut_edges_cost` from it. `cost()` may
read only the facts, the side and its members. An unassigned vertex (the value -1,
which ruin-and-recreate uses for a moment) is a member of neither side.

The defaults live in these two constants and nowhere else; Python reads them through
`max_cut.constraint_info()`. `kDefaultLevel` is `Hard` or `Soft`: this domain has no
medium level, and `level="medium"` raises `ValueError`. A hard constraint returns whole
numbers, because a hard contribution is rounded per side. A reward, a quantity to
maximize such as `cutWeightCost`, returns a raw cost of 0 or less and sets
`reward = true; level_flippable = false;` in its constructor: it can never be hard.

**2. The assembly**, `cpp/include/mc/constraints.hpp`: include the header, and add
one line to `defaultConstraints()`, after `PartitionSizeBoundsCost`. The catalog
lists the unit constraints first and the globals after them, and the docs write
`get_constraints()[:4]` for the four hard constraints:

```cpp
    assembly.emplace<UncutEdgesCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no default solve changes and the monolithic calculators stay as they are.
`partitionImbalanceCost` is such a constraint already.

**3. The Python spec**, `optimaru/max_cut/constraints.py`:

```python
class UncutEdgesCost(Constraint):
    """Edges with both endpoints on one side, counted. Opt-in: soft, weight 0."""

    name = "uncut_edges_cost"
    label_en = "Uncut edges"
    label_ko = "잘리지 않은 간선"
    description_en = (
        "Per side, the number of edges between two different vertices of that side, whatever their weight. "
        "One unit of cost is one edge left inside a side; 0 means every edge is cut. Its default weight is 0, "
        "so it only counts once you give it a weight."
    )
    description_ko = (
        "각 쪽에서 서로 다른 두 정점을 잇는 간선 가운데 양 끝이 모두 그 쪽에 놓인 간선의 수이며, 가중치는 세지 않습니다. "
        "한쪽 안에 남은 간선 하나가 비용 1이며, 0이면 모든 간선이 잘린 것입니다. "
        "기본 가중치가 0이므로 가중치를 주어야 점수에 반영됩니다."
    )
```

**4. Its default instance**, in the same file: `uncut_edges_cost = UncutEdgesCost()`
with the other instances, that instance in `_DEFAULTS` after
`partition_size_bounds_cost`, and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=max_cut
uv sync                                    # rebuilds optimaru.max_cut._core
```

```python
from optimaru.max_cut.constraints import UncutEdgesCost

others = [rule for rule in get_constraints() if rule.name != "uncut_edges_cost"]
picks = [*others, UncutEdgesCost(weight=2)]
config = max_cut.SolverConfig()
config.step_count_limit = 500_000
config.random_seed = 1
result = max_cut.solve(problem, config=config, constraints=picks)
print(max_cut.constraint_costs(result, picks)["uncut_edges_cost"]["costs"], result.cut_weight)
```

On g400 that prints `[377.0, 366.0] 9652`: 743 edges uncut. A default solve of the
same length (`constraints=None`) leaves `[395.0, 370.0]`, 765 edges, at a cut weight
of 9,603.

What the base gave you: the engine calls `cost()` for a side whenever a vertex joins
or leaves it, and keeps the result. That is correct, and slow: the function walks
every edge of the graph. Step 4 makes it incremental.

## 3. Check it

**A case worked out by hand**, in `tests/max_cut/cpp/mc_plugin_test.cpp`, with the
other components' cases in `verifyRawCostsByHand()`: a positive cost and the score it
gives at a weight, then a zero case and the edge cases (a self-loop never counts,
parallel edges count once each, an unassigned endpoint puts an edge inside no side).

```cpp
        // uncutEdgesCost: the edges inside each side, counted. It is opt-in,
        // so it scores at a weight.
        MaxCutProblem p = fixtureH();
        p.side = {0, 1, 1, 1};  // B-C and C-D inside side 1; A-B cut
        check(rawCosts(p, "uncutEdgesCost") == Costs({0.0, 2.0}),
              "uncutEdgesCost: [0, 2] (B-C, C-D on side 1)");
        DecomposedCalculator calculator;
        defaultConstraints().weight("uncutEdgesCost", 3).into(calculator);
        check(sameScore(scoreWith(calculator, p), Score{-5, 2.0 - 6.0}),
              "uncutEdgesCost at weight 3: 3 x 2 soft off the cut 2");
```

```sh
cmake -B build -DOPTIMARU_USE_CASES=max_cut && cmake --build build -j
ctest --test-dir build --output-on-failure
```

**The catalog.** A seventh constraint fails six checks of `mc_plugin_test` and eight
Python tests, and they list more than a count. Update them:

- `mc_plugin_test.cpp`, `expectedUnitCosts()` and `kCatalogNames` with the loops over
  them. The test compares every unit cost of the default assembly with the
  monolith's `RawCosts`, on 500 random partitions and after each of 3,000 random
  moves, twice. The monolith has no cost for an opt-in constraint, so compute one in the
  test from the facts (for every edge between two different vertices, add 1 to the
  side both are on) at the constraint's place in `expectedUnitCosts()`. The walk then
  checks the constraint after every move, and that it is above 0 somewhere on the way.
- `mc_plugin_test.cpp`, the names in `verifyConstraintTotals()`, and
  `rows.size() == 6`, the rows and `names()` in `verifyCatalog()`. Add the row after
  `partitionSizeBoundsCost`.
- `tests/max_cut/test_constraint_specs.py`, six tests: `NAMES`, the instances and the
  `constraint_info()` rows list the catalog,
  `test_constraint_costs_match_the_hand_calculation` lists every raw cost of its
  fixture, and `test_weight_none_means_the_component_default` names the one constraint it
  expected at weight 0 (read the default weights from `constraint_info()` there).
- `tests/max_cut/reference_max_cut.py`, the independent scorer in plain Python that
  two tests of `test_reference_parity.py` compare the engine with, per constraint.
  Add the constraint's name to `REFERENCE_NAMES` and `ENGINE_NAME`, its default to
  `REFERENCE_DEFAULTS`, and its terms to `constraint_costs()`, one per edge:

  ```python
          "uncut_edges": [
              int(edge["source"] != edge["target"] and partition.get(edge["source"]) == partition.get(edge["target"]))
              for edge in edges
          ],
  ```

Then add cases that pick the constraint, at a weight and on the hard level
(`test_levels_by_hand_on_both_paths` in `test_constraint_specs.py`).

**Incremental against full.** Solve with every score checked against a full
recompute. It throws `ScoreMismatchError` if a move leaves the score stale:

```python
max_cut.solve(problem, time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

Give the constraint a weight for this: at weight 0 a stale cost scores the same. The walk
of `mc_plugin_test` compares raw costs, so it does not depend on the weight.

**The tables and the CP-SAT model.** The constraint table in [README.md](README.md)
must equal `max_cut.constraint_info()`; add the row. [python.md](python.md),
`__init__.py` and `_core.pyi` describe the score in words, and the notebook
`examples/max-cut.ipynb` prints the constraint list. The benchmark compares against
`external_solver.max_cut` in
[external-solver](https://github.com/optimaru/external-solver): one plugin per
constraint, a subclass of the Python spec with its defaults from `constraint_info()`.
Its `tests/max_cut/test_constraint_parity.py` runs over `list_constraints()` and pins
partitions to compare each raw cost, so it fails for a constraint without a plugin, opt-in
or not. Mirror the constraint there with an exact encoding.

## 4. Make it cheaper per move

On the assembly a move costs, for every constraint in the picks, one call for the side a
vertex leaves and one for the side it joins. What that call is depends on the constraint:

| The constraint is | Use | Per move | Example to copy |
| --- | --- | --- | --- |
| a sum of one term per vertex of the side | `McUnitConstraint` with `cost()`, `supports_delta()` and `delta_cost()` | one lookup | [`fixed_partition_cost.hpp`](../../../optimaru/max_cut/cpp/include/mc/constraints/fixed_partition_cost.hpp) |
| a sum over pairs or edges inside a side | the same, with `delta_cost()` over the vertex's list | the vertex's pairs or edges | this step |
| a function of the side's size | `McUnitConstraint` with `cost()` alone | `members.size()` | [`partition_size_bounds_cost.hpp`](../../../optimaru/max_cut/cpp/include/mc/constraints/partition_size_bounds_cost.hpp) |
| one number across both sides | `detail::MemberChangeConstraint`: keep your own totals in `on_member_changed()` | what your update costs | [`cut_weight_cost.hpp`](../../../optimaru/max_cut/cpp/include/mc/constraints/cut_weight_cost.hpp) |

A unit constraint with `cost()` alone is correct: the engine then recomputes the
whole side on every change. For the example that is a pass over all 2,392 edges for
each of the two sides. `delta_cost()` returns what one vertex adds given the others,
and must equal `cost(with the vertex) - cost(without it)` exactly. `full_assert`
(step 3) checks that it does. Add to the component of step 2:

```cpp
    void on_reset(const MaxCutProblem& problem) override { detail::requirePrepared(problem); }
```

```cpp
    bool supports_delta() const override { return true; }
    double delta_cost(const MaxCutProblem& problem, int unit, const std::vector<int>& /*members*/,
                      int vertex) const override {
        double delta = 0.0;
        for (int k = problem.adjStart[vertex]; k < problem.adjStart[vertex + 1]; ++k) {
            if (problem.side[problem.adjVertex[k]] == unit) delta += 1.0;
        }
        return delta;
    }
```

With the constraint at weight 2 the assembly went from 0.24 to 3.42 million steps per
second, and the solve of step 2 printed the same line as before, as it must.
At weight 0 it went from 0.23 to 3.58 million: the engine keeps the cost of every
constraint in the picks, whatever its weight, and `get_constraints()` lists the opt-in
constraints too. So a constraint without `delta_cost()` slows every solve that lists it. On a
random 200-vertex instance of the test fixtures, 1 million steps called `cost()` 2.2
million times without the delta, and `delta_cost()` 5.0 million times with it.

Three things made the delta short, and they hold for any constraint here.

**Read the indexes that are built once.** `ScoreCalculator::prepare()` runs when a
solve starts and builds the adjacency of every vertex (`adjStart`, `adjVertex`,
`adjWeight`, without self-loops) and the pair lists (`sameAsFirst*`, `sameAsSecond*`,
`diff*`); see `MaxCutProblem` in
[`domain.hpp`](../../../optimaru/max_cut/cpp/include/mc/domain.hpp). `on_reset()`
checks that it ran. A constraint that needs another index builds it in `on_reset()`,
which runs when the objective is rebuilt: twice in a solve, at the start and for the
final score, and at every step under `full_assert`.

**Read the other vertices' sides from `problem.side`.** The engine reports a change
one vertex at a time, also inside a swap, so at every `delta_cost()` call each other
vertex is on the side `problem.side` says. `cost()` must not do that: it gets the
members and reads nothing else of the partition.

**Keep raw costs whole numbers.** Every constraint here counts vertices, pairs or edges,
so the sums stay exact. Return `static_cast<double>` of a count, or add `1.0`.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`max_cut.solve(problem)` must score it, and it scores with the monolithic
calculators: 1.7 times the steps per second of the assembly, and the only path that
runs parallel starts. `mc_plugin_test` compares the default assembly with them on
random partitions, so it fails from the moment the weight is not 0 until both sides
agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 1.0;`.
It must be a whole number, because the monolithic calculators add in `int64`.

**2. The term**, in four files. `RawCosts` in `cpp/include/mc/score_calculator.hpp`
holds the raw cost of every constraint from the full recompute. Add the field, and
`uncutTotal()` for the sum of the two sides:

```cpp
    std::int64_t uncut[kSideCount] = {0, 0};      // uncutEdgesCost
```

In `cpp/src/score_calculator.cpp` include the header, read the weight off the
component (`kUncutWeight`), and add it to the first `static_assert` and its level to
the second. Then the cost in `rawCosts()` and the term in `scoreOf()`:

```cpp
    // uncutEdgesCost: an edge between two different vertices of one side.
    for (const Edge& edge : problem.edges) {
        const int a = problem.sideOf(edge.source);
        if (edge.source != edge.target && a >= 0 && problem.sideOf(edge.target) == a)
            ++raw.uncut[a];
    }
```

```cpp
    score.soft = static_cast<double>(kCutWeight * raw.cut - kImbalanceWeight * raw.imbalance -
                                     kUncutWeight * raw.uncutTotal());
```

The incremental calculator keeps one running total per constraint. Add `uncut` to its
`Totals` and the member `uncut_` in `cpp/include/mc/incremental_calculator.hpp`. In
`cpp/src/incremental_calculator.cpp`: the same include, weight and `static_assert`,
`uncut_` set to 0 in `rebuild()` and copied in `totals()`, the same term in
`score()`, and the count in `apply()`, in the loop that already walks the vertex's
edges for the cut:

```cpp
    for (int k = p.adjStart[v]; k < p.adjStart[v + 1]; ++k) {
        const int w = side_[p.adjVertex[k]];
        if (w < 0) continue;
        if (w != s)
            cut_ += sign * p.adjWeight[k];
        else
            uncut_ += sign;
    }
```

**3. The parity checks.** `expectedUnitCosts()` in `mc_plugin_test.cpp` now reads
`raw.uncut`, its walk with the imbalance on subtracts `raw.uncutTotal()` from the
soft score it expects, and the row in `verifyCatalog()` has the weight 1.
`sameTotals()`, the names and the counts of `verifyIncrementalWalk()` in
`tests/max_cut/cpp/mc_test.cpp` get the seventh entry: that walk compares the
incremental calculator with the full recompute, total by total. The Python reference
gets the weight in `REFERENCE_DEFAULTS`, and
`test_constraint_info_reports_the_cpp_defaults` the row.

**4. What else moves.** With that done the parity checks pass on both sides, and
every check that states a score at the defaults fails:

- `mc_test`: 7 checks, the hand-computed scores. "triangle 0,1,0 cuts A-B and B-C:
  5" is now 4 soft, because A-C stays inside side 0;
- `mc_plugin_test`: 13 checks, in `verifyPartialAssemblies()`, `verifyOverrides()`
  and the assembly that `verifyAssemblyBuilder()` lists by hand;
- Python: 12 cases, in `test_constraint_specs.py` (5), `test_solve.py` (2),
  `test_models.py` (2), `test_max_cut.py`, `test_initial_solution.py` and
  `test_reference_parity.py` (1 each);
- the table and the counts in [README.md](README.md) and [python.md](python.md), and
  the constraint lists and counts in the comments of `common.hpp`, `constraints.hpp`,
  `score_calculator.hpp` and `domain.hpp` and in the docstrings of `constraints.py`,
  `__init__.py` and `_core.pyi`;
- the CP-SAT model: without the constraint at the same weight in `external_solver.max_cut`,
  [`benchmarks/max-cut`](../../../benchmarks/max-cut) no longer compares one
  objective. Its published results and `CHANGELOG.md` follow.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed:

```python
import time

steps = 10_000_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    core = problem.to_core()
    config = max_cut.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    max_cut.solve(core, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e6:5.2f} M steps/s  {core.score}")
```

Both lines end on the same score: same seed, same steps, same scoring. If they
differ after a change, the two paths no longer score alike.

| Measured on g400, median of three runs | Steps per second |
| --- | --- |
| default path, six constraints | 6.8 million |
| assembly, the five constraints that score by default | 4.06 million |
| assembly, those + the constraint of step 2 at weight 0 | 0.23 million |
| assembly, those + the constraint of step 2 at weight 2 | 0.24 million |
| assembly, those + the constraint with `delta_cost()` (step 4) at weight 0 | 3.58 million |
| assembly, those + the constraint with `delta_cost()` at weight 2 | 3.42 million |
| default path, seven constraints (step 5) | 8.35 million |
| assembly, the six constraints that score by default (step 5) | 3.75 million |

The two default-path rows are not the same work: with the constraint on, the solve makes
other moves.

**What it buys.** A constraint changes what the search can reach, so compare the results
at the same budget over several seeds, with and without it:

```python
for seed in range(1, 6):
    config = max_cut.SolverConfig()
    config.step_count_limit = 500_000
    config.random_seed = seed
    for picked in (others, picks):
        result = max_cut.solve(problem, config=config, constraints=picked)
        uncut = sum(max_cut.constraint_costs(result, picks)["uncut_edges_cost"]["costs"])
        print(seed, result.cut_weight, uncut)
```

Each line is the cut weight and the number of uncut edges, without the constraint and
then with it at weight 2. On g400 the constraint leaves fewer edges uncut on all five
seeds (765, 739, 752, 742 and 756 without it; 743, 738, 714, 731 and 748 with it),
and the cut weight moves both ways, from 46 less to 52 more.

Against CP-SAT at a fixed time: `uv run max-cut-compare --seconds 10 --workers 8` in
`benchmarks/max-cut/cpsat` ([README](../../../benchmarks/max-cut/README.md)). It
solves on the default path only, so it measures a constraint after step 5 and with the
constraint in the CP-SAT model.

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/mc/constraints/<subject>_cost.hpp` | new | | `on_reset`, delta | weight |
| `cpp/include/mc/constraints.hpp` | include, `emplace` | | | |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | counts in the text |
| `tests/max_cut/cpp/mc_plugin_test.cpp` | | case, costs, counts | | costs, row |
| `tests/max_cut/test_constraint_specs.py` | | lists, cases | | row, expected scores |
| `tests/max_cut/reference_max_cut.py` | | the constraint | | weight |
| `docs/use-cases/max-cut/README.md`, `python.md` | | table row | | defaults |
| `cpp/include/mc/score_calculator.hpp`, `incremental_calculator.hpp` | | | | the fields |
| `cpp/src/score_calculator.cpp`, `incremental_calculator.cpp` | | | | the term |
| `tests/max_cut/cpp/mc_test.cpp` | | | | totals, expected scores |
| external-solver, `benchmarks/max-cut` | | the same constraint | | results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.

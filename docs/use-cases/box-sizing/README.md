# Box sizing

Which box sizes should a warehouse stock? From the parcels it has shipped and a list of
candidate boxes, pick the box each parcel ships in so that only a few distinct boxes are in
use and the least air is shipped. Original to optimaru, built on this repository's C++ engine
(`core/optimaru/`).

A **parcel** is the packed content of one shipment, described by its bounding size, with the
number of shipments that had that size. It fits a box when its three sides, sorted, are each
at most the box's sorted sides, so it may be turned any way. Choosing the box for the items of
one order (how they are arranged inside it) is another problem; this one takes each
shipment's packed size as given.

## Usage

```sh
OPTIMARU_USE_CASES=box_sizing uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/box_sizing
```

```python
from optimaru import box_sizing as bs
from optimaru.box_sizing.models import BoxModel, BoxSizingProblemModel, ParcelModel

problem = BoxSizingProblemModel(
    boxes=[                                                    # the candidates, inner sizes in mm
        BoxModel(id="S", length=200, width=150, height=100),
        BoxModel(id="M", length=300, width=200, height=150),
        BoxModel(id="L", length=400, width=300, height=200, max_weight=20_000),
        BoxModel(id="XL", length=600, width=400, height=400, max_weight=30_000),
    ],
    parcels=[                                                  # what was shipped, weights in g
        ParcelModel(id="P1", length=180, width=140, height=60, weight=400, quantity=320),
        ParcelModel(id="P2", length=150, width=280, height=120, weight=900, quantity=210),
        ParcelModel(id="P3", length=380, width=250, height=180, weight=4_500, quantity=45),
        ParcelModel(id="P4", length=550, width=350, height=300, weight=12_000, quantity=8),
    ],
    max_box_types=3,                                           # room on the shelf for three sizes
)
solved = bs.solve(problem, time_limit_seconds=2.0, random_seed=42)   # a new model; `problem` is unchanged
print(solved.score_hard, solved.box_types_used, solved.fill_ratio)   # 0 3 0.4191...
print({parcel.id: parcel.box for parcel in solved.parcels})          # {'P1': 'M', 'P2': 'M', 'P3': 'L', 'P4': 'XL'}
print({box.id: box.parcels for box in solved.boxes})                 # {'S': 0, 'M': 530, 'L': 45, 'XL': 8}
```

`solve` takes the pydantic model and returns a new, solved one, or takes the compiled
`BoxSizingProblem` and solves it in place; the score is on the result (`score_hard`,
`score_soft`, or `problem.score`). It takes `time_limit_seconds`, `random_seed`,
`parallel_solver_count`, `stop_at_first_feasible`, `full_assert`, `on_new_best`, a full
`config` (`box_sizing.SolverConfig`) and `constraints` (see below).
`bs.generate_demo_data()` builds a demo instance: 24 candidate sizes, 120 parcel sizes, six
box types. The C++ side builds and tests with:

```sh
cmake -B build -DOPTIMARU_USE_CASES=box_sizing && cmake --build build -j
ctest --test-dir build
```

## Domain (assignment shape)

- `Parcel.box` is the only decision variable: an index into `boxes`, `-1` = unassigned (in
  the JSON model: the box's `id`, or `null`). A parcel with `quantity` 40 is 40 shipments
  that all go in the same box size.
- `Box` is a problem fact: a candidate size (`length`, `width`, `height`, the inner size),
  the heaviest parcel it carries (`max_weight`, optional), what one box costs (`cost`, 0 by
  default) and whether it is already stocked and staying (`required`: it counts as in use
  even when no parcel takes it).
- `max_box_types` is how many distinct boxes may be in use, the required ones included; no
  limit when it is left out.
- Lengths and weights are whole numbers in whatever unit you use; the examples here use mm
  and g. See [Units](#units).
- Shipping prices, for `shipping_cost` (opt-in): a box may carry `dim_divisor`, and a parcel
  in it then bills at least the box's **dimensional weight**, `ceil(box volume /
  dim_divisor)` (5000 in mm and g is 1 kg per 5 litres, the usual metric divisor). A
  shipment's **billed weight** is the larger of its own weight and that. `rate_tables`
  prices it: one table per zone, each a list of weight breaks (`max_weight`, `price`,
  `max_weight` strictly ascending), and a shipment pays the price of the first break that
  covers its billed weight. A parcel's `zone` picks its table; a parcel without one is
  priced by the table without a zone.

```json
"rate_tables": [
  {"breaks": [{"max_weight": 2000, "price": 4.0}, {"max_weight": 5000, "price": 5.5},
              {"max_weight": 30000, "price": 11.0}]},
  {"zone": "remote", "breaks": [{"max_weight": 5000, "price": 9.0}, {"max_weight": 30000, "price": 18.0}]}
]
```

A solve refuses a parcel that fits none of the candidate boxes (by size and by `max_weight`),
with a `ValueError` that describes the first such parcel and names up to five more, and a
`max_box_types` below the number of required boxes: no assignment could be feasible. The
model refuses them when it is built; the engine refuses them again for a compiled problem.
With rate tables, a repeated zone, breaks out of order, a parcel whose zone has no table
(also when there are no tables at all) and, at solve time, a parcel that some box takes at a
billed weight above its table's top break are refused too: past its last break a table says
nothing, so end it with a break that covers the heaviest billed weight.

The solved model carries the outputs: `box_types_used`, `void_volume` (the sum over the
parcels of `quantity x (box volume - parcel volume)`), `fill_ratio` (shipped parcel volume /
shipped box volume), `board_area` (the sum over the parcels of `quantity x 2 (lw + lh + wh)`
of their box) and, on every box, `parcels` (the shipments in it: quantities summed). Where
weight is billed (some box has a `dim_divisor`, or there are rate tables) every box also
reports `billed_weight` (over its parcels, `quantity x` the billed weight of a shipment), and
with rate tables `shipping_cost` (over its parcels, `quantity x` the price of a shipment),
with the total `shipping_cost` on the problem. They are reported whatever the constraints
weigh: at the default weights the shipping cost is a report, not an objective.
On an assignment that is not feasible, a parcel larger than its box counts as the box's
volume in both: no air, and a fill of 1 for that shipment, so `fill_ratio` never passes 1.

## Constraints

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `parcel_fit_cost` | `ParcelFitCost` | hard | 1 | per box: over its parcels, the length by which the parcel exceeds the box, summed over the three sorted sides |
| `box_weight_cost` | `BoxWeightCost` | hard | 1 | per box: over its parcels, the parcel's weight above the box's `max_weight` |
| `void_volume_cost` | `VoidVolumeCost` | soft | 1 | per box: over its parcels, `quantity x (box volume - parcel volume)` |
| `box_cost` | `BoxCost` | soft | 1 | per box: over its parcels, `quantity x` the box's `cost` |
| `board_area_cost` | `BoardAreaCost` | soft | 0 (opt-in) | per box: over its parcels, `quantity x 2 (lw + lh + wh)`: the board of the boxes shipped |
| `shipping_cost` | `ShippingCost` | soft | 0 (opt-in) | per box: over its parcels, `quantity x` the price of a shipment's billed weight in the rate table of its zone; 0 without rate tables |
| `box_type_limit_cost` | `BoxTypeLimitCost` | hard | 1 | the distinct boxes in use (the required ones included) above `max_box_types`: one number |
| `box_type_cost` | `BoxTypeCost` | soft | 0 (opt-in) | the distinct boxes in use (the required ones included): one number |

Every constraint is flippable (`level="hard"` / `level="soft"`) and none is a reward. Five are
scored by default; `board_area_cost`, `shipping_cost` and `box_type_cost` have weight 0 and
score nothing until they are picked with a weight (`default_enabled` is `False`), so a
request that leaves them alone solves as it did before they existed.
`box_sizing.constraint_info()` reports exactly this table from the C++ components
(`kDefaultLevel` / `kDefaultWeight`), in this order: the six per-box constraints, then the
two that count the boxes in use.

- The two hard per-box costs are not multiplied by the quantity: they say how far a size is
  from fitting, not how many shipments had it. A parcel is one shipment in one box, so the
  weights of a box's parcels are not added up against `max_weight`.
- `void_volume_cost` counts 0 for a parcel that is larger than its box (which does not fit
  it), not a negative volume, so the cost is never a bonus. On every feasible solution it is
  `quantity x (box volume - parcel volume)` as written.
- **`box_type_cost` is the fixed cost of a size.** A weight on it is what stocking one more
  size costs, in the units of the soft level: the solve stops adding a size once it saves
  less than that. With mm, 2,000,000 says a size is worth it when it saves two litres of air
  over the shipments of the problem. It works with or without `max_box_types`.
- **`box_cost` shares the soft level with the void volume**, so its weight says how much
  volume one unit of cost is worth. A `cost` in a currency next to a void in mm³ needs a
  weight: at the default 1 a box that costs 300 more is avoided when it saves less than 300
  mm³ per shipment, which is nothing.
- **`board_area_cost` is the material.** Of two boxes that hold the same, the cube takes
  less board than the flat one, and a box a size too large takes more of both: a weight on
  it says how much air one mm² of board is worth (50 says a square metre of board costs as
  much as 50 litres of air). Choosing sizes by the board used rather than by the air shipped
  is the objective box-set studies on warehouse data have found closer to practice.
- **`shipping_cost` is what the parcels cost to send.** A shipment pays the price of its
  billed weight in its zone's table, so a box one size too large can cost a whole break more
  for a light parcel, and two boxes that bill in the same break cost the same however much
  air they ship. Its weight says how much air one unit of money is worth; to minimise the
  shipping cost alone, pick it with `void_volume_cost` and `box_cost` left out (then the
  smaller box wins a tie). Without rate tables it is 0.

The problem carries no weights: they come only from constraint selections.

```python
from optimaru.box_sizing.constraints import BoxCost, BoxTypeCost, get_constraints

picks = [
    BoxTypeCost(weight=2_000_000) if spec.name == "box_type_cost" else spec   # a size must save 2 litres
    for spec in get_constraints()
]
bs.solve(problem, time_limit_seconds=2.0, constraints=picks)
```

Besides `weight`, `level` and `exponent`, every constraint takes `offset` and `clip`
(keyword-only), and may be picked several times. A raw cost `x` is shaped as
`sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`); `offset=0` is the plain shaping.
The shaping applies per box for the four per-box constraints, and to the one count for the
two others (so `BoxTypeCost(weight=w, exponent=2)` charges `w x count²`). `clip=False` with
an offset is refused on hard, because hard 0 must mean no violation. Each pick is its own
instance with its own knobs, and the instances add up;
`bs.constraint_costs(problem, picks)` keys a second pick `void_volume_cost#2`.

With a selection, the construction and the moves price the sizes with the picked weights
of `void_volume_cost`, `box_cost`, `board_area_cost`, `shipping_cost` and `box_type_cost`, so
a zero-budget solve already follows them (see [Search](#search)).

The problem of [Usage](#usage), billed: every box at divisor 5000, one rate table, and the
shipping cost as the only objective.

```python
from optimaru.box_sizing.constraints import ShippingCost, get_constraints
from optimaru.box_sizing.models import RateBreakModel, RateTableModel

priced = problem.model_copy(deep=True)
for box in priced.boxes:
    box.dim_divisor = 5000                              # S bills at least 600 g, M 1,800, L 4,800, XL 19,200
priced.rate_tables = [RateTableModel(breaks=[
    RateBreakModel(max_weight=w, price=p) for w, p in [(1000, 4.0), (2000, 5.5), (5000, 7.0), (20_000, 12.0), (30_000, 15.0)]
])]
shipping_only = [                                       # what the parcels cost to send, nothing else
    ShippingCost(weight=1) if spec.name == "shipping_cost" else spec
    for spec in get_constraints()
    if spec.name not in ("void_volume_cost", "box_cost")
]
cheap = bs.solve(priced, time_limit_seconds=2.0, random_seed=42, constraints=shipping_only)
print({parcel.id: parcel.box for parcel in cheap.parcels})   # {'P1': 'S', 'P2': 'M', 'P3': 'XL', 'P4': 'XL'}
print(cheap.shipping_cost)                                   # 3071.0 (3326.0 for the plan of the least air)
print({box.id: box.billed_weight for box in cheap.boxes})    # {'S': 192000, 'M': 378000, 'L': 0, 'XL': 1017600}
```

The light `P1` bills 600 g in `S` (4.0) against 1,800 g in `M` (5.5), which over 320 shipments
is worth more than what `P3` pays in `XL` instead of `L` (12.0 against 7.0, 45 times): so `S`
takes the third place on the shelf and `L` gives it up, for a third more air.

### Units

Volumes are whole numbers and the soft level is a double, which holds whole numbers
exactly up to 2^53 (about 9 x 10^15). A solve refuses a problem whose shipped volume could
pass that: the summed quantities times the largest box volume. In mm that is 9 million
shipments in a 1 m³ box; with more, use cm. A side is at most 2,000,000 and a weight at most
10^12. Where weight is billed (a `dim_divisor` or rate tables) the summed quantities times the
heaviest billed weight must stay below 2^53 as well, so that `billed_weight` is exact.

The default path adds the soft level up exactly, also with a fractional `cost`. A solve
with a constraint selection keeps one running soft total, as every use case does: a
fractional `cost` next to a void of 10^12 is then kept to about 10^-4, which the result
does not notice (the final score is a full recompute) but `full_assert`, which compares to
10^-6, can report. Whole costs, or a coarser length unit, avoid it.

## C++

- `optimaru/box_sizing/cpp/bs/model/domain.hpp`: domain + `optimaru::SolutionTraits` specialization;
  `model/pricing.hpp`: what a box costs a parcel, for the construction and the moves
- `optimaru/box_sizing/cpp/bs/constraints/`: one component per constraint: `parcel_fit_cost.hpp`,
  `box_weight_cost.hpp`, `void_volume_cost.hpp`, `box_cost.hpp`, `board_area_cost.hpp`,
  `shipping_cost.hpp`, `box_type_limit_cost.hpp`, `box_type_cost.hpp` (+ `common.hpp`); `optimaru/box_sizing/cpp/bs/constraints.hpp` assembles
  the default set (`defaultConstraints()`)
- `score/score_calculator`: full recompute (the reference), and the checks of the facts
- `score/incremental_calculator`: before/after hooks, O(1) per change
- `construct/construction`: the greedy over the sizes
- `search/moves/`: `CloseBoxMove`, `OpenBoxMove`, `SwapBoxMove` (and their base, `open_set_move.hpp`)
- `accept/reheating_late_acceptance.hpp`: late acceptance that starts over just below its best
- `solver`: the engine wrapper
- `optimaru/box_sizing/cpp/main.cpp`: a demo instance (12 candidates, 36 parcel sizes, 4 box types)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPTIMARU_USE_CASES=box_sizing
cmake --build build -j4
./build/box_sizing/bs_test         # per-constraint hand checks, refusals, the construction, the moves, the acceptor, optima of small instances (also priced by board and shipping)
./build/box_sizing/bs_plugin_test  # plugin assembly == monolith, raw costs by hand, catalog, overrides, box_type_cost
./build/box_sizing/bs_demo 2       # 2 s solve; prints the sizes to stock (exit 0 when feasible)
```

## Python

pybind11 package `box_sizing` (module `optimaru.box_sizing._core`); [python.md](python.md)
has the compiled objects, the JSON request and the settings.

## Search

Which sizes are in use is a step function of the assignment: a size stops counting only
when its last parcel leaves, and a size worth opening pays off only once every parcel it
serves cheaper has moved to it. Moving one parcel does neither. So the search works on the
**set of sizes**, and places the parcels for a set in the same step. Given the set, placing
needs no search: among the boxes that take a parcel, the cheapest is the one with the lowest
`void weight x volume + cost weight x cost + board weight x board`, the same order for every
parcel. A shipping price depends on the parcel too (its weight and zone against the box's
dimensional weight), so with a weight on `shipping_cost` the parcels that the same boxes take
and that each of those boxes prices at the same break of the same table form a group, and
each group orders its boxes by that price plus its shipping price (ties to the smaller box).

- **Construction** (`construct/construction.cpp`): a greedy. The set starts as the
  required boxes (and the boxes a seed ships in). While a parcel is taken by no size of the
  set, it adds the size that takes the most such parcels; then it adds, one at a time, the
  size with the largest saving (what the parcels that would move to it save, less the
  fixed cost of a size when `box_type_cost` has a weight), until `max_box_types` is reached
  or no size saves anything. `time_limit_seconds=0` returns this construction as it is.
  The cover is greedy and so not exact: with candidates that do not nest (a tall box and a
  flat one) it can use up the limit and leave a parcel for one size more, although another
  set within the limit takes them all. The search then starts one size over the limit, and
  closing that size is one `CloseBoxMove`.
- **`CloseBoxMove`** (`custom_moves.close_box`) takes one size out of use and sends each of
  its parcels to the cheapest remaining size that takes it.
- **`OpenBoxMove`** (`custom_moves.open_box`) puts one more size into use and pulls in every
  parcel it serves cheaper. No move at `max_box_types`.
- **`SwapBoxMove`** (`custom_moves.swap_box`) opens a size and closes another in one step:
  at the limit, the only way to another set. The size to close is the one whose closing
  costs the least once the new size is open (one pass over the parcels finds it for all
  sizes at once), and one time in four a size drawn at random.
- **Acceptance** (`acceptance.reheat`): late acceptance explores for as long as its history
  holds scores worse than the current one, which it does on the way down from a poor start.
  From the greedy construction it holds nothing worse, and the search is a descent to the
  first set that no single move improves. So after two histories without an improving step
  the acceptor refills its history with a score just below the best one seen (0.5%, 1%, 2%,
  4% and 8% of the soft score, in turn): the search walks through sets that are worse by up
  to that gap and descends again. It never takes a step that gives up feasibility, and it
  counts in steps, so a seeded solve with a step limit is reproducible.

The engine's change and swap moves and these three take turns evenly (the adaptive
selection of the other use cases rewards a move by its outcome, and here a move on the set
is turned down most of the time and is still the only kind that reaches another set). The
ruin-and-recreate kicks are off by default (`alns_trigger.enabled`): a kick re-places the
ruined parcels one at a time, and each goes back to the size it had.

### How good, how fast

Measured on the 20 instances of [`benchmarks/box-sizing`](../../../benchmarks/box-sizing)
(the demo instance and 19 generated warehouses, 40 parcel sizes against 12 candidates up to
8,000 against 300), one thread of an Apple M4 Max, 2026-11-28.

**Against the exact optimum.** On the eight smallest (up to 300 parcel sizes, 40 candidates,
six box types) OR-Tools CP-SAT proves the optimum, in under 0.01 s to 11 s on 4 workers. The
search returns that optimum on all eight, from each of three seeds, and has found it within
2 ms of the start. The greedy construction alone is optimal on four of them and ships
0.3%, 1.6%, 2.4% and 5.0% more air than the optimum on the others.

**Against the greedy construction, and CP-SAT's best at 30 s.** On the twelve larger
instances CP-SAT proves nothing within 30 s, so the comparison is with the construction
(what a zero budget returns) and with the best solution CP-SAT has after 30 s on 4 workers.
Air shipped above the search's result at 30 s, the mean over the four instances of a size:

| Instances | Greedy construction | CP-SAT, 30 s |
| --- | ---: | ---: |
| 1,000 parcel sizes, 80 candidates, 8 box types | +1.1% (0 to +4.2%) | +1.5% (0 to +3.2%) |
| 3,000 parcel sizes, 150 candidates, 10 box types | +2.1% (+0.2% to +3.6%) | +8.6% (+4.8% to +11.1%) |
| 8,000 parcel sizes, 300 candidates, 12 box types | +3.6% (+2.6% to +5.3%) | +29.6% (+15.4% to +49.4%) |

The search never returns more air than its construction. A budget of 2 s gives the result
of 30 s on 19 of the 20 instances, and 0.19% more air on one of the largest (the mean of
three seeds).

**What each part is worth.** The same instances at 2 s, three seeds: the air shipped above
the default search's, the mean over the instances of a size.

| Instances (parcel sizes) | No moves on the set of sizes | No `SwapBoxMove` | No reheating |
| --- | ---: | ---: | ---: |
| 40 (three instances) | +0.79% | +0.79% | 0 |
| 120 (the demo) | +1.63% | +1.63% | 0 |
| 300 | +1.32% | +1.32% | 0 |
| 1,000 | +1.06% | +1.06% | +0.05% |
| 3,000 | +2.08% | +1.32% | 0 |
| 8,000 | +3.50% | +3.42% | +0.07% |

Without the three moves the search returns the construction: the change and swap moves of
single parcels improve nothing, as the section above says. Nearly all of the gain is the
swap; closing and opening alone recover a third of it on the instances of 3,000 parcel sizes
and next to nothing elsewhere. The reheating finds what is left on four instances of 1,000
and 8,000 parcel sizes, which end 0.03% to 0.21% higher without it. With the
ruin-and-recreate kicks switched on, 19 instances end on the same result and one of the
largest moves by 0.05%, a third of what its three seeds differ by.

**Speed.** The construction takes 0.1 ms on the demo instance and 35 ms on 8,000 parcel
sizes against 300 candidates. The search makes 1.9 million steps per second on the demo
instance, 250 thousand on 1,000 parcel sizes and 25 thousand on 8,000: a step on the set of
sizes reads every parcel once (parcels that the same boxes take are placed together), so a
step costs what the instance holds.

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose search starts
from `solution`: a solved model, or that model as a JSON dict.

```python
restart = bs.with_initial_solution(problem, solved)        # sets warm_start on the copy
again = bs.solve(restart, time_limit_seconds=2.0, random_seed=43)
```

Parcels are matched by id, and a parcel's `box` is a box id, so the solution may list the
boxes in another order. A parcel the solution leaves out, or holds without a box, is placed
around the others by the construction (the sizes the seed uses are open from the start), and
a complete solution is exactly where the search starts. An unknown or repeated parcel id and
a `box` that is not one of the problem's candidates raise `ValueError`.

A model's own `box` values are the same thing: with `warm_start=True` a solve starts from
them, and without it a solve ignores them. So a solved model can be solved again from
scratch as it is. A typical use: last quarter's sizes as the seed, this quarter's parcels as
the problem.

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **assignment** shape; the domain solver that configures the
engine is `optimaru/box_sizing/cpp/bs/solver.hpp` + `optimaru/box_sizing/cpp/bs/solver.cpp`.

**Adding a move.** The three moves on the set of sizes derive from `OpenSetMove`
(`bs/search/moves/open_set_move.hpp`): a move reads the solution (`census()`), changes the
planned set (`planned_`) and calls `commit()`, which places every parcel in its cheapest
box of that set and remembers how to undo it. A move that decides on the set another way
(close the two smallest sizes, open the size nearest to a parcel that fits badly) is a
dozen lines on that base, registered in `registerMoves()` of `solver.cpp` behind a switch
of the `Config`.

**Changing the construction heuristic.** `constructGreedy()` in
`bs/construct/construction.cpp` runs before the engine; the engine only fills what is still
unassigned, which is nothing.

**Adding a hard or soft cost.** [Adding a constraint, step by step](extending.md) takes one new
constraint through this use case's files in six steps: changed from Python, written as a
component, tested, made cheaper per move, moved to the default fast path and measured.

## Benchmark

[`benchmarks/box-sizing`](../../../benchmarks/box-sizing) compares this use case with OR-Tools
CP-SAT on the same instances; the CP-SAT model is in that directory.

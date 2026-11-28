# optimaru.box_sizing (Python)

The pybind11 package of the box-sizing use case (C++ in `optimaru/box_sizing/cpp`).
Picks one candidate box per parcel: `Parcel.box` is the only decision variable (a box
index, -1 = unassigned; in the JSON model the box's id, or `null`).

Score, at the default weights (1 each):

- hard = -(length by which parcels exceed their box, over the three sorted sides
  + parcel weight above the box's `max_weight`
  + distinct boxes in use above `max_box_types`):
  `parcel_fit_cost`, `box_weight_cost`, `box_type_limit_cost`
- soft = -(`quantity x (box volume - parcel volume)` + `quantity x` the box's `cost`, over the
  parcels): `void_volume_cost`, `box_cost`

`box_type_cost` (the distinct boxes in use, soft) has weight 0: it scores nothing until it
is picked with a weight, which is then the fixed cost of stocking one more size.

| Constraint (Python name) | C++ component | Default level | Default weight |
|---|---|---|---|
| `parcel_fit_cost` | `ParcelFitCost` | hard | 1 |
| `box_weight_cost` | `BoxWeightCost` | hard | 1 |
| `void_volume_cost` | `VoidVolumeCost` | soft | 1 |
| `box_cost` | `BoxCost` | soft | 1 |
| `box_type_limit_cost` | `BoxTypeLimitCost` | hard | 1 |
| `box_type_cost` | `BoxTypeCost` | soft | 0 (opt-in) |

All six are flippable and none is a reward. The problem carries no weights. Pick, re-weight
or flip constraints with `optimaru.box_sizing.constraints`, and read the defaults with
`bs.constraint_info()`:

```python
from optimaru import box_sizing as bs
from optimaru.box_sizing.constraints import BoxCost, BoxTypeCost, VoidVolumeCost, get_constraints, score

bs.constraint_info()
# [ConstraintInfo(name='parcel_fit_cost', default_level='hard', default_weight=1.0,
#                 level_flippable=True, reward=False, default_enabled=True), ...]
problem = bs.generate_demo_data().to_core()
picks = [BoxTypeCost(weight=2_000_000) if spec.name == "box_type_cost" else spec for spec in get_constraints()]
bs.solve(problem, time_limit_seconds=1.0, constraints=picks)   # every size must save 2 litres of air
raw = bs.constraint_costs(problem)        # raw costs per box (and the two counts), re-priced in pure Python:
score(raw), score(raw, [VoidVolumeCost(), BoxCost(weight=1000)])
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping per box (clip=True: only the void beyond 50 litres per box size counts).
score(raw, [VoidVolumeCost(), VoidVolumeCost(weight=1e-9, exponent=2, offset=50_000_000)])
```

## Install and test

```bash
OPTIMARU_USE_CASES=box_sizing uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/box_sizing
```

## Usage

The model is the usual way in: `solve` returns a new, solved model.

```python
from optimaru import box_sizing as bs

problem = bs.generate_demo_data()                 # models.BoxSizingProblemModel: 24 boxes, 120 parcel sizes
solved = bs.solve(problem, time_limit_seconds=2.0, random_seed=42)
print(solved.score_hard, solved.score_soft)       # 0 -2767496500.0
print(solved.box_types_used, solved.void_volume, round(solved.fill_ratio, 3))   # 6 2767496500 0.511
print([(box.id, box.parcels) for box in solved.boxes if box.parcels])
```

The compiled objects solve in place:

```python
problem = bs.BoxSizingProblem(
    [bs.Box("S", 200, 150, 100), bs.Box("L", 400, 300, 200, max_weight=20_000, cost=0.0, required=False)],
    [bs.Parcel("P1", 180, 140, 60, weight=400, quantity=320), bs.Parcel("P2", 380, 250, 180, weight=4_500)],
    max_box_types=2,
)
bs.solve(problem, time_limit_seconds=1.0, random_seed=42)
print(problem.score)                  # 0hard/-483060000soft
print(problem.assignment())           # box index per parcel: [0, 1]
print(problem.box_types_used(), problem.void_volume(), problem.fill_ratio(), problem.parcels_per_box())
# 2 483060000 0.509... [320, 1]
```

Note: `problem.parcels[i]` returns a copy. Write boxes in bulk with
`problem.set_assignment([...])`, and set `problem.warm_start = True` to make the next solve
start from them (a plain solve ignores the boxes on the parcels).

For finer control build `bs.SolverConfig()` (time budget, parallel starts, the moves on the
set of sizes `custom_moves.close_box` / `open_box` / `swap_box` with their gate
`custom_moves.share`, `acceptance.reheat` and `acceptance.late_acceptance_size`, the
`on_new_best` callback, `stop.target_score` early stop, ...; see the guide's
[Search](README.md#search)) and call `bs.Solver(cfg).solve(problem)` or
`bs.solve(problem, config=cfg)`. `stop.time_limit_seconds = 0` returns the greedy
construction.

## JSON in, JSON out

`optimaru.box_sizing.models` is the JSON contract:

```
BoxSizingProblemModel
    boxes: list[BoxModel]                 at least one: the candidate sizes
    parcels: list[ParcelModel]
    max_box_types: int | None = None      >= 1: how many distinct boxes may be in use
    warm_start: bool = False              start from the parcels' `box`
    # outputs
    score_hard, score_soft
    box_types_used: int                   distinct boxes in use, the required ones included
    void_volume: int                      sum over the parcels of quantity * (box volume - parcel volume)
    fill_ratio: float                     shipped parcel volume / shipped box volume

BoxModel
    id: str
    length, width, height: int            > 0: the inner size
    max_weight: int | None = None         the heaviest parcel it carries
    cost: float = 0.0                     >= 0: what one box of this size costs
    required: bool = False                already stocked and staying: counts as in use
    # output
    parcels: int | None                   the shipments in it (quantities summed)

ParcelModel
    id: str
    length, width, height: int            > 0
    weight: int = 0
    quantity: int = 1                     >= 1 (at most 2^53): how many shipments had this size
    box: str | None = None                in and out: the id of the box it ships in
```

An unknown field is refused. So are a repeated id, a `box` that is not a candidate's id, a
parcel that fits none of the candidates (by size and `max_weight`) and a `max_box_types`
below the number of required boxes, each with a message that names it.

`solve_request` takes a pydantic request, `models.BoxSizingRequest`: the problem model as
`model_input`, with the settings of the solve under the names of `solve`'s keywords
(`time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`,
`full_assert`, and `solver_parameters` for further `SolverConfig` settings by group, such as
`{"acceptance": {"reheat": false}}`). It returns the solved `BoxSizingProblemModel`, as
`solve(model, ...)` does, and leaves the request unchanged. `constraints=` is the selection
`solve` takes.

```python
from optimaru.box_sizing import solve_request
from optimaru.box_sizing.models import BoxSizingRequest

request = BoxSizingRequest.model_validate_json(text)  # or BoxSizingRequest(model_input=model, time_limit_seconds=5.0)
solved = solve_request(request)
```

On the optimaru API the use case is `box-sizing`, and the request body is the Python call:
the model as `problem`, and the keywords of `solve`.

## Parameters and their costs

A request is refused when it sets a field that acts only through a cost while its
constraints leave that cost out ([Parameters and their costs](../README.md#parameters-and-their-costs)):

| Field | Its cost |
| --- | --- |
| box `max_weight` | `box_weight_cost` |
| box `cost` | `box_cost` |
| box `required` | `box_type_limit_cost` or `box_type_cost` |
| `max_box_types` | `box_type_limit_cost` |

The default selection holds all of them (`box_type_cost` at weight 0 counts: a weight of 0
is an explicit choice). `models.parameter_uses(model)` lists what a model sets.

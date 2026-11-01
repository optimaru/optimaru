# Bed allocation CP-SAT

OR-Tools CP-SAT formulation using the canonical `BedPlanModel`. It preserves
the hard/medium/soft ordering, supports the no-bed sentinel, and re-scores the
result with the shared engine evaluator.

## Constraints

`bed_allocation_cpsat/constraints.py` holds one plugin per engine constraint, named
like the domain spec it subclasses (`BedConflictCost`, ..., `UnassignedStaysCost`, ...,
`MixedGenderRoomCost`), in `list_constraints()` order. A plugin carries only `build()`:
its default level and weight come from `bed_allocation.constraint_info()` (the C++
components' `kDefaultLevel` / `kDefaultWeight`), so `catalog()` equals the engine catalog.
The objective levels are `("hard", "medium", "soft")`; `unassigned_stays_cost` is medium
at weight 1, `level="hard"` makes it hard and `level="soft"` soft, exactly like the engine
(0.2.0: a demoted medium term used to stay on medium here). Not supported on CP-SAT:
`exponent != 1` and non-integer weights (rejected).

## Engine comparison

The comparison runner is owned by this use case. It builds deterministic
instances, gives both backends the same solve budget, and re-scores both answers
with the engine evaluator.

```sh
uv run bed-allocation-compare --seconds 10
uv run bed-allocation-compare --instance demo --seconds 2 --json
```

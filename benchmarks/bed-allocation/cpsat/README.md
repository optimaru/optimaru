# Bed allocation CP-SAT

OR-Tools CP-SAT formulation using the canonical `BedPlanModel`. It preserves
the hard/medium/soft ordering, supports the no-bed sentinel, and re-scores the
result with the shared engine evaluator.

## Constraints

[`external_solver.bed_allocation.constraints`](https://github.com/optimaru/external-solver/tree/main/src/external_solver/bed_allocation/constraints.py) holds one plugin per engine constraint, named
like the domain spec it subclasses (`BedConflictCost`, ..., `UnassignedStaysCost`, ...,
`MixedGenderRoomCost`), in `list_constraints()` order. A plugin carries only `build()`:
its default level and weight come from `bed_allocation.constraint_info()` (the C++
components' `kDefaultLevel` / `kDefaultWeight`), so `catalog()` equals the engine catalog.
The objective levels are `("hard", "medium", "soft")`; `unassigned_stays_cost` is medium
at weight 1, and `level="hard"`, `"medium"` or `"soft"` (or a `Level`) moves any pick onto
that level with its weight in units of it, exactly like the engine. Not supported on CP-SAT: `exponent != 1` and
non-integer weights (rejected).

**Repeats, `offset` and `clip`.** A constraint may be picked several times (as specs
or as `{"name", "weight", "level", "offset", "clip"}` mappings): each pick builds its own
terms with its own knobs, its level included, keyed `name`, `name#2`, … like the engine's
`constraint_costs()`. Every plugin adds one term per engine unit (per real bed, the "no
bed" sentinel for `unassigned_stays_cost`, one total for `mixed_gender_room_cost`), so an
integral offset is shaped exactly as in the engine, in both modes; a fractional offset is
refused (`offset must be an integer for CP-SAT`). `clip=False` with an offset is refused on
hard with the engine's message (`constraint 'bed_conflict_cost': clip=False with a non-zero
offset cannot be hard, ...`); medium folds into soft, as in the engine, so it takes the
literal formula. `tests/test_solve.py` pins all 27
assignments of a tiny instance and checks that the CP-SAT objective, the engine's own score
and `bed_allocation.constraints.score()` agree per level under repeated, shifted and
re-levelled picks.

## Engine comparison

The comparison runner is owned by this use case. It builds deterministic
instances, gives both backends the same solve budget, and re-scores both answers
with the engine evaluator.

```sh
uv run bed-allocation-compare --seconds 10
uv run bed-allocation-compare --instance demo --seconds 2 --json
```

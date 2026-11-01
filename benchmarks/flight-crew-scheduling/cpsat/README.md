# Flight crew CP-SAT

CP-SAT backend for the canonical flight-crew model. It optimizes skills,
availability, overlapping duties, chronological airport transfers, home-base
starts/ends, and pinned assignments. The engine evaluator supplies the final
reported score.

## Constraints

One plugin per engine constraint, in `flight_crew.list_constraints()` order
(`required_skill_cost`, `flight_conflict_cost`, `transfer_between_two_flights_cost`,
`employee_unavailability_cost`, `first_assignment_not_departing_from_home_cost`,
`last_assignment_not_arriving_at_home_cost`). Each plugin subclasses the domain spec of
the same name (`flight_crew.constraints.RequiredSkillCost`, ...) and only adds `build()`;
its default level and weight come from `flight_crew.constraint_info()`, so the two
solvers cannot disagree on a default. `level="soft"` demotes a constraint to the soft level
and `level="hard"` promotes one to hard, in both solvers; `level="medium"` is refused on
both (`this use case has no medium level`). Not supported on CP-SAT (refused):
`exponent != 1` and non-integer weights.

**0.3.0:** a constraint may be picked several times (as specs or as
`{"name", "weight", "level", "offset", "clip"}` mappings); each pick builds its own terms
with its own knobs, its level included. Every plugin adds one term per employee, an idle
one included, so an integral `offset` shapes each employee exactly as the engine does in
both `clip` modes; a fractional offset is refused (`offset must be an integer for
CP-SAT`).

## Engine comparison

The comparison runner is owned by this use case. It builds deterministic
instances, gives both backends the same solve budget, and re-scores both answers
with the engine evaluator.

```sh
uv run flight-crew-compare --seconds 10
uv run flight-crew-compare --instance demo --seconds 2 --json
```

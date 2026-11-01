# optimaru

A metaheuristic optimization engine for real-world routing, scheduling,
timetabling and allocation problems — with 22 ready-to-use models.

The solver core is C++17 and ships compiled in the wheel; you use it from
Python. Every business rule and preference is a constraint *plugin*: switch it
on or off, change its weight, make it hard or soft, or pick it several times
with different settings — without touching the solver.

```bash
pip install optimaru
```

Requires Python 3.11+ (wheels for CPython 3.11–3.14). Wheels are published for Linux, macOS and Windows.

## Example: vehicle routing

```python
from optimaru import vehicle_routing as vrp
from optimaru.vehicle_routing.constraints import (
    TravelTimeCost, UnassignedVisitsCost, VehicleCapacityCost, get_constraints,
)

locations = [vrp.Location(37.5665, 126.9780)]          # 0: depot
visits = []
for i, (lat, lon) in enumerate([(37.5700, 126.9820), (37.5512, 126.9882), (37.5133, 127.1001)]):
    locations.append(vrp.Location(lat, lon))
    #            id       name           location demand  ready      due        service (s)
    visits.append(vrp.Visit(f"C{i}", f"Customer {i}", i + 1, 2, 8 * 3600, 20 * 3600, 600))

vehicles = [vrp.Vehicle("V1", 6, 0, departure_time=7 * 3600)]
plan = vrp.VehicleRoutePlan("demo", locations, vehicles, visits)

config = vrp.SolverConfig()
config.spent_limit_seconds = 2
solver = vrp.Solver(config)

solver.solve(plan)                                      # default constraints
print(plan.score, plan.routes())

# Pick constraints and reweight them — the solver itself does not change.
# Only the listed constraints are scored: leave out UnassignedVisitsCost() and
# skipping every visit becomes free.
solver.solve_with_constraints(plan, get_constraints([
    VehicleCapacityCost(),
    UnassignedVisitsCost(),
    TravelTimeCost(weight=10),
]))
print(plan.score, plan.routes())

# Pick one constraint twice: every driving second, plus the square of the
# seconds a route drives beyond 10 minutes.
solver.solve_with_constraints(plan, get_constraints([
    VehicleCapacityCost(),
    UnassignedVisitsCost(),
    TravelTimeCost(),
    TravelTimeCost(exponent=2, offset=600),
]))
print(plan.score, plan.routes())
```

Output:

```
0hard/0medium/-966soft {'V1': ['C0', 'C1', 'C2']}
0hard/0medium/-9660soft {'V1': ['C0', 'C1', 'C2']}
0hard/0medium/-134922soft {'V1': ['C0', 'C1', 'C2']}
```

The last score is the 966 driving seconds plus `(966 - 600)**2 = 133956`.

Every constraint takes the same five knobs:

- `weight`: `None` is its default weight.
- `level`: `None` is its default level, otherwise a `Level` or its name, such as
  `Level.MEDIUM` or `"medium"`.
- `exponent`: `1.0` by default; it shapes the cost of each unit (a vehicle, an
  employee, ...) as a sign-preserving power.
- `offset` and `clip` (keyword-only): an offset shifts that shaping. With
  `clip=True`, the default, it is a threshold: `TravelTimeCost(offset=10,
  exponent=3)` costs `max(0, x - 10)**3` for a vehicle that drives `x` seconds.
  `clip=False` is the literal formula, `(x - 10)**3`, which is negative, a
  reward, below 10.

A constraint may be picked several times. Each pick is its own instance with its
own knobs, and the instances add up: `TravelTimeCost(exponent=2)` and
`TravelTimeCost(exponent=3)` together cost `x**2 + x**3`. The examples write
every pick as a class call; the `constraints` module also has ready-made default
instances such as `vehicle_capacity_cost`, which is `VehicleCapacityCost()`.
`constraint_info()` on a use case lists its constraints with their default level
and weight.

## Use cases

| Family | `optimaru.…` |
| --- | --- |
| Routing & logistics | `vehicle_routing`, `pickup_delivery`, `order_picking`, `facility_location` |
| Scheduling | `employee_scheduling`, `flight_crew_scheduling`, `maintenance_scheduling`, `project_job_scheduling`, `semiconductor_scheduling`, `food_packaging`, `task_assigning`, `meeting_scheduling` |
| Timetabling | `school_timetabling`, `conference_scheduling`, `sports_league_scheduling`, `tournament_scheduling` |
| Allocation & packing | `bed_allocation`, `bin_packing`, `knapsack`, `assembly_line_balancing`, `quadratic_assignment`, `max_cut` |

Each subpackage is loaded on first use, so `import optimaru` stays cheap.
`list_constraints()` on a use case shows the constraint plugins it ships with,
and `constraint_info()` their defaults.

## Links

- Website: <https://optimaru.com>
- Source, docs and benchmarks against OR-Tools: <https://github.com/optimaru/optimaru>
- Example notebooks, one per use case: <https://github.com/optimaru/optimaru/tree/main/examples>

optimaru comes in a free, open-source edition (this package, Apache-2.0) and
an Enterprise edition (coming soon).

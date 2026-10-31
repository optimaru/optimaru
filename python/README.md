# optimaru

A metaheuristic optimization engine for real-world routing, scheduling,
timetabling and allocation problems — with 22 ready-to-use models.

The solver core is C++17 and ships compiled in the wheel; you use it from
Python. Every business rule and preference is a constraint *plugin*: switch it
on or off, change its weight, or make it hard or soft — without touching the
solver.

```bash
pip install optimaru
```

Requires Python 3.11+ (wheels for CPython 3.11–3.14). Wheels are published for Linux, macOS and Windows.

## Example: vehicle routing

```python
from optimaru import vehicle_routing as vrp
from optimaru.vehicle_routing.constraints import (
    MinimizeTravelTime, get_constraints, maximize_visits_assigned, vehicle_capacity,
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
# Only the listed constraints are scored: leave out maximize_visits_assigned and
# skipping every visit becomes free.
solver.solve_with_constraints(plan, get_constraints([
    vehicle_capacity,
    maximize_visits_assigned,
    MinimizeTravelTime(weight=10),
]))
print(plan.score, plan.routes())
```

## Use cases

| Family | `optimaru.…` |
| --- | --- |
| Routing & logistics | `vehicle_routing`, `pickup_delivery`, `order_picking`, `facility_location` |
| Scheduling | `employee_scheduling`, `flight_crew_scheduling`, `maintenance_scheduling`, `project_job_scheduling`, `semiconductor_scheduling`, `food_packaging`, `task_assigning`, `meeting_scheduling` |
| Timetabling | `school_timetabling`, `conference_scheduling`, `sports_league_scheduling`, `tournament_scheduling` |
| Allocation & packing | `bed_allocation`, `bin_packing`, `knapsack`, `assembly_line_balancing`, `quadratic_assignment`, `max_cut` |

Each subpackage is loaded on first use, so `import optimaru` stays cheap.
`list_constraints()` on a use case shows the constraint plugins it ships with.

## Links

- Website: <https://optimaru.com>
- Source, docs and benchmarks against OR-Tools: <https://github.com/optimaru/optimaru>

optimaru comes in a free, open-source edition (this package, Apache-2.0) and
an Enterprise edition (coming soon).

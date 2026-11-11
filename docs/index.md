# optimaru

A metaheuristic optimization engine for real-world routing, scheduling,
timetabling and allocation, with 22 ready-to-use models you call from Python. The solver core is
header-only C++17 and ships compiled in the wheel.

## Quick start

```bash
pip install optimaru
```

One package with every use case as a subpackage — `optimaru.vehicle_routing`,
`optimaru.school_timetabling`, … — compiled for Linux, macOS and Windows
(Python 3.11+). No C++ toolchain is needed.

```python
from optimaru import vehicle_routing as vrp
from optimaru.vehicle_routing.constraints import (
    TravelTimeCost, UnassignedVisitsCost, VehicleCapacityCost, get_constraints,
)

# Depot at index 0, then three customers.
locations = [vrp.Location(37.5665, 126.9780), vrp.Location(37.5700, 126.9820),
             vrp.Location(37.5512, 126.9882), vrp.Location(37.5133, 127.1001)]
#                    id    name  location demand ready     due        service (s)
visits = [vrp.Visit(f"C{i}", f"C{i}", i + 1, 2, 8 * 3600, 20 * 3600, 600) for i in range(3)]
vehicles = [vrp.Vehicle("V1", 6, 0, departure_time=7 * 3600)]
plan = vrp.VehicleRoutePlan("demo", locations, vehicles, visits)

config = vrp.SolverConfig()
config.spent_limit_seconds = 2
solver = vrp.Solver(config)

solver.solve(plan)                      # default constraints
print(plan.score, plan.routes())        # 0hard/0medium/-966soft {'V1': ['C0', 'C1', 'C2']}

# Pick and reweight constraints; the solver does not change.
solver.solve_with_constraints(plan, get_constraints([
    VehicleCapacityCost(),
    UnassignedVisitsCost(),
    TravelTimeCost(weight=10),
]))
print(plan.score)                       # 0hard/0medium/-9660soft
```

Every constraint takes the same knobs — `weight`, `level` (`"hard"`,
`"medium"`, `"soft"`), `exponent`, `offset` and `clip` — and may be picked
several times. `constraint_info()` on a use case lists its constraints and
their defaults. More in the [use-case guides](use-cases/README.md) and the
[notebooks](https://github.com/optimaru/optimaru/tree/main/examples).

## Where to go next

- **[Use cases](use-cases/README.md)**: the 22 models, the constraint contract
  they share (names, levels, the `weight` / `level` / `exponent` / `offset` /
  `clip` knobs), and one guide per use case.
- **[API reference](reference/vehicle-routing.md)**: generated from the package
  sources and the stubs of the compiled extensions, one page per use case.
- **[Extending a use case](extending.md)**: adding a constraint (a hard, medium
  or soft cost) or a move, step by step. Every use case has a worked example
  of its own, "Adding a constraint" under its name. Contributors start with
  [CONTRIBUTING.md](https://github.com/optimaru/optimaru/blob/main/CONTRIBUTING.md).
- **[Changelog](changelog.md)**.

The source, the benchmarks against OR-Tools and the notebooks are on
[GitHub](https://github.com/optimaru/optimaru).

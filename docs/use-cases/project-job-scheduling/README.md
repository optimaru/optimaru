# Project job scheduling

Derived from the Timefold Quickstarts `project-job-scheduling` example (Apache-2.0), translated
and modified; see the repository [NOTICE](../../../NOTICE).

Domain model, constraints and API details: [`python.md`](python.md).

## Usage

```sh
OPTIMARU_USE_CASES=project_job_scheduling uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/project_job_scheduling
```

```python
from optimaru import project_job_scheduling as pj
from optimaru.project_job_scheduling.demo import generate_demo_data

schedule = generate_demo_data()
result = pj.solve(schedule, time_limit_seconds=5.0, random_seed=42)
print(result.score_hard, result.score_medium, result.score_soft)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`, `full_assert`, `on_new_best`, a full `config` (`optimaru.project_job_scheduling.SolverConfig`) and `constraints` (see below). The config has the switch and the gate of the domain's own move, `serial_schedule_moves` and `custom_move_probability` ([Search](#search)). The C++ side builds and tests with:

```sh
cmake -B build -DOPTIMARU_USE_CASES=project_job_scheduling && cmake --build build -j
ctest --test-dir build
```

`pjs_test` hand-checks the monolithic score, incremental ==
full recompute and a ScoreChecks::Full solve; `pjs_plugin_test` checks the default assembly ==
the monolith, the raw costs by hand (one positive and one zero case per constraint), the
catalog and the medium knobs.

## Constraints

The score has three levels, hard / medium / soft, folded into the engine's `Score`.

`optimaru/project_job_scheduling/cpp/include/pjs/constraints/` holds one component per constraint
(`non_renewable_resource_capacity_cost.hpp`, `renewable_resource_capacity_cost.hpp`,
`project_delay_cost.hpp`, `makespan_cost.hpp`, plus `common.hpp`);
`optimaru/project_job_scheduling/cpp/include/pjs/constraints.hpp` assembles the default set (`defaultConstraints()`).
All four are global constraints (each reads the whole precedence chain or the whole
stock), so each has one raw total, and the exponent shapes that total.

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `non_renewable_resource_capacity_cost` | `NonRenewableResourceCapacityCost` | hard | 1 | resource units of total use above capacity, summed over the non-renewable resources |
| `renewable_resource_capacity_cost` | `RenewableResourceCapacityCost` | hard | 1 | resource-unit-days of use above capacity, summed over (renewable resource, day) |
| `project_delay_cost` | `ProjectDelayCost` | medium | 1 | days the SINK of each project ends after its critical-path end date (release + critical path duration), summed over the projects |
| `makespan_cost` | `MakespanCost` | soft | 1 | the latest SINK end date, an absolute day index (one value for the schedule, a max, not a sum) |

`list_constraints()` and `constraint_info()` report them in this order;
`optimaru.project_job_scheduling.constraint_info()` reports exactly this table from the C++ components
(`kDefaultLevel` / `kDefaultWeight`). Every constraint is flippable onto any of the three
levels (`level="hard"`, `"medium"` or `"soft"`, or a `Level`), none is a reward and all four
are scored by default (`default_enabled`).

The medium weight is in **medium units**: the domain folds the medium level into the
engine's soft level as `engine.soft = medium * 10^9 + soft` (`kMediumFold` in
`optimaru/project_job_scheduling/cpp/include/pjs/domain.hpp`, reported as `medium_fold: {"kind": "into_soft", "unit": 1e9}`
on every `constraint_costs()` entry). `level="hard"` makes
`project_delay_cost` 1 hard unit per day, `level="soft"` 1 soft unit per day (outside the
fold), and a `weight` is in units of the level in effect — in the
engine and in the CP-SAT reference alike. Any other constraint may move onto medium the
same way (`MakespanCost(level="medium")` charges one medium unit per day of the makespan,
in both solvers too), and a repeated pick carries its own level.

`makespan_cost` is the latest end date (a max), not a total. `project_delay_cost` defaults to
weight 1 on the medium level; the fold belongs to the domain, so an explicit weight is in medium
units, and `level="soft"` on a hard constraint lands on soft in both solvers.

Besides `weight`, `level` and `exponent`, every constraint takes `offset` and `clip`
(keyword-only), and may be picked several times. All four constraints are global, so each instance shapes its
one total `x` once: `sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default)
or `(x - offset) ** exponent`, sign-preserving (`clip=False`, which acts as a reward below
the offset, e.g. a makespan under a target day); `offset=0` is the plain shaping.
`clip=False` with an offset is refused on hard (the two resource constraints by default),
because hard 0 must mean no violation; medium folds into soft here, so it takes the literal
formula. Each pick is its own instance with its own knobs, and the instances add up:

```python
pj.solve(core, constraints=[
    *get_constraints(),
    MakespanCost(exponent=2, offset=40),   # + (days beyond day 40)^2
])
```

`pj.constraint_costs(core, picks)` keys the second pick `makespan_cost#2`. The domain
`Score` keeps integer levels, so fractional weights round in `schedule.score`;
`constraints.score()` reports the exact engine value. The CP-SAT reference takes integral
offsets on every constraint (one term per global total).

## Search

The construction places the allocations one after another; late acceptance then works with
the engine's change / swap moves on single values (a mode and a delay), its
ruin-and-recreate kicks, and one move of the domain, `SerialScheduleMove`
(`serial_schedule_moves`, in `optimaru/project_job_scheduling/cpp/include/pjs/moves.hpp`).

A delay counts from the end of an allocation's predecessors, so one changed delay shifts
every successor. In a schedule whose allocations already start as early as they can, almost
no single change keeps the resources within capacity and ends earlier, and a search with
the built-in moves alone stalls some percent above what the instance allows.
`SerialScheduleMove` works on the order of the schedule instead (the serial schedule
generation scheme of resource-constrained project scheduling): it reads the start dates,
changes the order they give, and places every allocation again in that order, each as early
as its predecessors and the renewable resources allow. One move makes one of these changes:

- one allocation moves next to another, or the two trade places: mostly a few places
  apart, now and then anywhere in the order;
- a few neighbours in the order trade places at random;
- a group moves by a number of days: an allocation with everything before it, or after it,
  in the precedence graph, or a whole project;
- one allocation switches its execution mode, half the time together with a second one of
  its project (where a non-renewable resource is at its capacity, one mode can take more of
  it only when another gives some back);
- none: the allocations are pushed to the right, as late as their successors allow, and
  placed again in the order of those starts (a forward-backward pass).

With several projects, the ones that end after their critical path come first in the
order, and half the moves pick an allocation of such a project: `project_delay_cost` is
scored above `makespan_cost`, and a project that is not late loses nothing by waiting for
one that is.

A rebuilt schedule is within every renewable capacity, with two exceptions: a delay that
reaches the end of `delay_range_size`, and a pinned allocation, which keeps its delay and
takes its resources where that puts it. Non-renewable capacities depend on the modes alone
and are left to the score. The move makes a few passes over the schedule where a built-in
move changes one value, so `custom_move_probability` (0.2) caps how often a step draws it;
`serial_schedule_moves = False` gives the search of 0.5.1 and earlier, step for step.

Measured on one thread (2026-11-12), the move off and on:

| Instances | off | on |
| --- | --- | --- |
| RG30 Pat1–Pat5, makespan after 30 s (OR-Tools CP-SAT on 8 workers: 62 / 74 / 55 / 73 / 71) | 66 / 82 / 59 / 76 / 75 | 63 / 75 / 55 / 73 / 72 |
| the 20 generated instances of that shape, 30 to 120 activities, after 30 s | makespans add up to 5,138 | 4,044 (21% less): shorter on all 20 |
| the 20 instances of the comparison ladder, after 30 s | | better on 9, the same on 11 |
| the 300-job instance of the ladder after 20,000 steps (a second or less), three seeds: project delay | 6,476 to 7,108 days | 15 to 31 days |
| from a given plan, 100,000 steps: three sizes of the ladder, a rough plan and a solved plan with a fifth of its allocations taken out, 12 seeds each | | better on 56 of the 72, the same on 15, worse on 1 |

The tables of [`benchmarks/project-job-scheduling`](../../../benchmarks/project-job-scheduling)
have the 30 s runs instance by instance, beside CP-SAT's.

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose search starts from `solution`, which is the model a solve returned or that model as a JSON dict:

```python
solved = pj.solve(schedule, time_limit_seconds=5.0, random_seed=42)
seeded = pj.with_initial_solution(schedule, solved.model_dump(mode="json"))
result = pj.solve(seeded, time_limit_seconds=5.0, random_seed=43)
print(result.score_hard, result.score_medium, result.score_soft)
```

Allocations are matched by id. An allocation the solution leaves out is placed by the construction heuristic, and a complete solution is exactly where the search starts. Pinned allocations, which always include SOURCE and SINK, keep their value on `problem`, whatever the solution says. `ValueError` is raised for an unknown id, for a mode that is not one of the job's, for a delay without a mode and for a delay outside the delay range, before anything reaches the engine, which does not range-check.

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **assignment** shape; the domain solver that configures the
engine is `optimaru/project_job_scheduling/cpp/include/pjs/solver.hpp` + `optimaru/project_job_scheduling/cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`), and the use case adds one move of its own, `SerialScheduleMove`
([Search](#search)). A new move is a
`optimaru::MoveOperator<…>` registered with `engine.add_move_operator(...)` next
to the engine construction in `optimaru/project_job_scheduling/cpp/src/solver.cpp`, as
`SerialScheduleMove` is; a ruin for LNS is a
`RuinOperator` registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** Entities are placed in index order (First Fit). Set
`engineConfig.construction.difficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** [Adding a constraint, step by step](extending.md) takes one new
constraint through this use case's files in six steps: changed from Python, written as a
component, tested, made cheaper per move, moved to the default fast path and measured.

## Benchmark

[`benchmarks/project-job-scheduling`](../../../benchmarks/project-job-scheduling) compares this use case with OR-Tools CP-SAT on the same instances.

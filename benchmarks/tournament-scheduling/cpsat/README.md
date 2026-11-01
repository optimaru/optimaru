# Tournament scheduling CP-SAT

CP-SAT backend for day/team assignment. Hard day collisions and
unavailability are optimized first, followed by squared team-load and
confrontation-load balancing. The final canonical unfairness score is computed
by the shared engine evaluator.

## Constraints and parity with the engine

One plugin per engine constraint (`tournament_scheduling_cpsat/constraints.py`),
each a subclass of the domain spec with only `build()`. Default levels and
weights are not restated here: cpsat-common reads them from
`tournament_scheduling.constraint_info()`, so they are the C++ components'
`kDefaultLevel` / `kDefaultWeight` (`test_cpsat_defaults_are_the_engine_defaults`).
The model has three levels, `("hard", "medium", "soft")`; `hard=False` demotes a
hard or medium constraint to **soft** and `hard=True` promotes to hard, exactly as
in the engine (`test_hard_false_demotes_to_soft_on_cpsat_too`). The reported
score folds medium into soft with the engine's `MEDIUM_SCALE` (1e6).

| Constraint | Level, weight | CP-SAT objective term | Raw-identical to the engine? |
|---|---|---|---|
| `team_conflict_cost` | hard, 1 | one 0/1 term per pair of a team's assignments on one day | **yes** |
| `team_unavailability_cost` | hard, 1 | the (team, day) occupancy bool of each unavailability fact | **yes** |
| `assignment_count_imbalance_cost` | medium, 1 | `scale * ssd` of the per-team loads | **no** (surrogate) |
| `confrontation_count_imbalance_cost` | soft, 1 | `scale * ssd` of the per-pair confrontation counts | **no** (surrogate, and see below) |

`test_the_hard_constraints_are_raw_identical` checks the first two rows against
`constraint_costs()` on fixed layouts (positive and zero cases).

The two imbalance constraints are **not** raw-identical. The engine's raw cost is
the LoadBalance unfairness `round6(sqrt(ssd))`, `ssd = sum(load^2) - sum(load)^2 / n`
over the `n` items with a load of at least 1, rounded to 6 significant figures.
CP-SAT has no square root, so `_add_unfairness_surrogate` adds the integral
variance numerator `scale * ssd`, with `scale = min(lcm(1..N), cap)` over the
`N` candidate items (teams, or team pairs) and `cap` keeping every product
inside int64; where the active count does not divide `scale`, the term is
`ceil(scale * ssd)` instead. Precisely:

- **The value differs.** Loads `{1, 4, 2}` cost `2.16025` in the engine and
  `28 = 6 * 14/3` in CP-SAT (`test_the_imbalance_surrogates_are_not_raw_identical`).
  An explicit weight multiplies the surrogate, `exponent != 1` is refused (as for
  every CP-SAT plugin).
- **The order agrees only for the term on its own, and only where both count
  the same loads.** `sqrt` and the 6-significant-figure rounding are monotone,
  so the surrogate alone ranks layouts like the engine and has the same
  minimizers, up to near-ties the rounding merges or the `ceil` (or a capped
  `scale`) introduces
  (`test_a_single_imbalance_surrogate_orders_feasible_layouts_like_the_engine`).
  For `assignment_count_imbalance_cost` the loads are always the same. For
  `confrontation_count_imbalance_cost` they are the same only on layouts where
  no team plays twice on a day it shares with another team (next bullet). That
  holds when `team_conflict_cost` is on the hard level of a hard-feasible
  instance. Without it, or on a hard-infeasible instance, the CP-SAT optimum
  need not be the engine optimum
  (`test_confrontation_alone_can_miss_the_engine_optimum`).
  `test_each_constraint_matches_brute_force_independently` (each constraint
  alone reaches the brute-force engine optimum) runs on an instance with two
  slots a day, where a team that plays twice on a day plays alone, so it cannot
  show the multiplicity difference. Summed with other terms on the same level —
  both imbalance costs demoted or promoted onto one level, an imbalance cost
  promoted to hard next to `team_conflict_cost`, or under a non-default weight —
  a sum of squares trades off differently than a sum of square roots, so the
  CP-SAT optimum need not be the engine optimum either.
- **Confrontation multiplicity.** The CP-SAT confrontation count of a team pair
  is the number of days on which both teams play; the engine counts every
  same-day assignment pair (`count(t1, d) * count(t2, d)`). The two agree
  whenever no team plays twice on one day (`team_conflict_cost == 0`), and also
  while a team that plays twice on a day is alone that day; otherwise they can
  differ: two assignments of T0 and one of T1 on a day are 2 confrontations in
  the engine and 1 in CP-SAT
  (`test_confrontations_count_once_per_shared_day_on_cpsat`).

The reported score never uses the surrogate: `solve()` rescores the solution
through the engine's `constraint_costs()` under the same selection, so a CP-SAT
answer and an engine answer are always compared in the engine's units
(`test_knobs_score_like_the_engine`, `test_default_score_folds_medium_like_the_engine`).

## Engine comparison

The comparison runner is owned by this use case. It builds deterministic
instances, gives both backends the same solve budget, and re-scores both answers
with the engine evaluator.

```sh
uv run tournament-scheduling-compare --seconds 10
uv run tournament-scheduling-compare --instance demo --seconds 2 --json
```

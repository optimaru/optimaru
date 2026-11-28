# How a solve runs, in the code

Every use case runs the same loop: build a first solution, then improve it one
move at a time, each move judged by an acceptor, with ruin and recreate when the
search stalls, until a limit is reached. This page follows that loop step by step
and says, for each step, where it is in the code, which settings change it, and
how to add to it. The steps and their names are those of the flowchart on
[optimaru.com](https://optimaru.com/en/solutions/engine/).

The loop itself is `Solver::run` in
[`core/optimaru/solver.hpp`](../core/optimaru/solver.hpp). It holds no move: every
operator it draws on is a plugin under [`core/optimaru/search/`](../core/optimaru/search/),
the acceptance rules are in [`core/optimaru/accept/`](../core/optimaru/accept/) and the
operator selection in [`core/optimaru/select/`](../core/optimaru/select/). The built-in
operators and acceptors use the same interfaces a use case registers its own with.
Each step below is marked in `Solver::solve` (step 1) and `Solver::run` (the rest) with a comment that names it
(`// How it solves, 3: ...`), so you can find it there by its number.

Paths are relative to the repository root. Settings are named as Python reads
them (`config.<group>.<name>`, see `optimaru/_config.py`); the C++ `Settings` in
[`core/optimaru/settings.hpp`](../core/optimaru/settings.hpp) has the same groups apart
from `cost`, which a use case reads itself (`stop` is of type `Limits` there).

| Step | Where | To add one |
| --- | --- | --- |
| [1. Construction](#1-construction) | `construct/construction.hpp` | [Construction instead of search](extending.md#construction-instead-of-search) |
| [2. Current and best](#2-current-and-best) | `solver.hpp`, `model/snapshot.hpp` | |
| [3. Stop condition](#3-stop-condition) | `stop/stop_condition.hpp` | |
| [4. Search stalled?](#4-search-stalled) | `solver.hpp`, `accept/history_stages.hpp` | |
| [5. Ruin and recreate kick](#5-ruin-and-recreate-kick) | `search/ruin_recreate/` | [A ruin instead of a move](extending.md#a-ruin-instead-of-a-move) |
| [6. Built-in strategy](#6-built-in-strategy) | `search/strategies.hpp`, `search/moves/` | an engine change |
| [7. Use-case moves](#7-use-case-moves) | `search/operators.hpp`, `select/` | [Adding a move](extending.md#adding-a-move) |
| [8. Move mix](#8-move-mix) | `search/moves/move_mix.hpp` | an engine change |
| [9. Incremental score](#9-incremental-score) | `model/editor.hpp`, `score/` | [Adding a cost](extending.md#adding-a-cost) |
| [10. Acceptor](#10-acceptor) | `accept/` | `Solver::set_acceptor_factory`, or an engine change |
| [11. New best](#11-new-best) | `solver.hpp` | |

All paths in the table are under `core/optimaru/`. A use case adds moves, ruins
and costs in its own files, `optimaru/<uc>/cpp/<ns>/`, and never edits these.

## Before the loop

### 1. Construction

Each unassigned entity gets its best placement; a solution passed in is kept and
only the rest is built. Assignment shape: the best value for each entity.
Sequence shape: cheapest insertion, the best list and position for each element.

- Code: `Construction<Solution>::run` in
  [`core/optimaru/construct/construction.hpp`](../core/optimaru/construct/construction.hpp),
  called by `Solver::solve` before `Solver::run`.
- Settings: `config.construction` (where the use case has it, such as
  `first_fit_decreasing`). In C++, `ConstructionSettings::difficulty` orders the
  entities (first fit decreasing).
- To change it for one use case: a difficulty order or a constructor of the use
  case's own ([Construction instead of search](extending.md#construction-instead-of-search)).

### 2. Current and best

The current solution `s` and the best one `s_best` both start from the
construction's. The best is kept as a snapshot apart from the solution the search
works on, so a step that makes things worse never loses it, and it is put back
when the solve ends.

- Code: the start of `Solver::run` in
  [`core/optimaru/solver.hpp`](../core/optimaru/solver.hpp), and `SolutionSnapshot` in
  [`core/optimaru/model/snapshot.hpp`](../core/optimaru/model/snapshot.hpp).
- Settings: `config.run.on_new_best` hears every new best.

## Each step, until a stop condition holds

### 3. Stop condition

Checked at the head of every step: a time limit, a step limit, a time or a number
of steps without a new best, or a target score. The first that holds ends the
solve. Long strategies ask the same condition between batches of evaluations, so
one move cannot run far past the time limit.

- Code: `StopCondition::should_stop` in
  [`core/optimaru/stop/stop_condition.hpp`](../core/optimaru/stop/stop_condition.hpp);
  the throttle long moves use is in
  [`stop/stop_throttle.hpp`](../core/optimaru/stop/stop_throttle.hpp).
- Settings: `config.stop` (`time_limit_seconds`, `step_limit`,
  `unimproved_time_limit_seconds`, `unimproved_step_limit`, `target_score`).

### 4. Search stalled?

The search has stalled when no new best has come for as many steps as it usually
takes between new bests (eight times their moving average), and at least for the
configured trigger. Every kick doubles the wait, up to 4096 times the trigger, and a new
best resets it, so the wait grows only while kicks find nothing. Where late acceptance runs in stages of growing history, a stall can
start the next stage instead of a kick.

- Code: the `patience` and `stalled` lines of `Solver::run` in
  [`core/optimaru/solver.hpp`](../core/optimaru/solver.hpp), and `HistoryStages`
  in [`core/optimaru/accept/history_stages.hpp`](../core/optimaru/accept/history_stages.hpp).
- Settings: `config.alns_trigger` (`enabled`, `unimproved_steps`). The stages are
  `AcceptanceSettings::history_growth` in C++, which a use case sets in its
  solver where it measured that they pay (facility location and quadratic
  assignment, for two).

### 5. Ruin and recreate kick

An adaptive selector picks a ruin operator; it removes part of the solution (10 to
30% by default), a greedy insertion rebuilds it (regret-k with k = 3 by default:
the element that would lose most by waiting goes in first; plain cheapest insertion
with `regret_k` below 2), and a short hill
climb with the move mix polishes the seams. The result becomes the current
solution whatever it scores; how it scored only teaches the selector which ruins
pay off.

- Code: `StagnationKick::apply` in
  [`core/optimaru/search/ruin_recreate/kick.hpp`](../core/optimaru/search/ruin_recreate/kick.hpp);
  the ruin operators and their selection in
  [`ruin_pool.hpp`](../core/optimaru/search/ruin_recreate/ruin_pool.hpp) (random,
  related, worst, and for sequences SISR and route ruins, one file each beside it);
  the rebuild in [`greedy_repair.hpp`](../core/optimaru/search/ruin_recreate/greedy_repair.hpp);
  the selection schemes in [`core/optimaru/select/`](../core/optimaru/select/).
- Settings: `config.alns` (`min_ruin_fraction`, `max_ruin_fraction`,
  `max_ruin_count`, `polish_steps`, `selection_type`, `rewards`, ...).
- To add a ruin for one use case: a `RuinOperator` registered with
  `add_ruin_operator()`; it joins the built-in ones in the same pool
  ([A ruin instead of a move](extending.md#a-ruin-instead-of-a-move)).

### 6. Built-in strategy

The step's move is asked for in a fixed order (`Solver::choose_move`). First, each
built-in strategy gets one draw by its weight, in priority order, and the first
that hits makes the move. A strategy searches a large neighbourhood in one move:
route elimination, ejection chain and iterated local search (these three on
sequences only), descent, and ruin and recreate as an ordinary move, which the
acceptor judges like any other.

- Code: `BuiltinStrategies::draw` in
  [`core/optimaru/search/strategies.hpp`](../core/optimaru/search/strategies.hpp);
  the strategies in [`core/optimaru/search/moves/`](../core/optimaru/search/moves/)
  (`descent.hpp`, `ejection_chain.hpp`, `iterated_local_search.hpp` with the
  descent it runs in `granular_local_search.hpp`, `route_elimination.hpp`) and
  [`search/ruin_recreate/ruin_recreate_move.hpp`](../core/optimaru/search/ruin_recreate/ruin_recreate_move.hpp).
- Settings: `config.strategies` (`*_weight` turns one on, the rest tune it).
- A new built-in strategy is a `MoveOperator` in `search/moves/` and a line in
  `BuiltinStrategies`: an engine change (see
  [Changing the engine](../CONTRIBUTING.md#changing-the-engine)).

### 7. Use-case moves

If the use case adds moves of its own, then on a set share of the steps left an
adaptive selector picks one of them or the move mix, by what each has found so
far.

- Code: `Solver::choose_move` and `Solver::custom_operators_get_a_turn` in
  [`core/optimaru/solver.hpp`](../core/optimaru/solver.hpp); the interface,
  `MoveOperator`, in [`core/optimaru/search/operators.hpp`](../core/optimaru/search/operators.hpp);
  the selection schemes in [`core/optimaru/select/`](../core/optimaru/select/).
  A use case's own moves are in `optimaru/<uc>/cpp/<ns>/search/moves/`, registered in
  its `registerMoves()` in `solver.cpp`.
- Settings: `config.custom_moves` (`share`, and the use case's switches).
- To add one: [Adding a move](extending.md#adding-a-move). This is the most common
  contribution, and it never touches the engine.

### 8. Move mix

Otherwise one ordinary small move is drawn by relative weight. Assignment shape:
change (another value for one entity) and swap. Sequence shape: relocate, swap,
2-opt, Or-opt, 2-opt* (tail swap) and CROSS-exchange. What it moves is chosen by
the selection rules: nearby candidates first where a distance is configured, and
the use case's entity and value hints.

- Code: `MoveMixSampler::sample` in
  [`core/optimaru/search/moves/move_mix.hpp`](../core/optimaru/search/moves/move_mix.hpp),
  which draws among the moves, one file each in the same folder: `change.hpp`,
  `swap.hpp`, `relocate.hpp`, `element_swap.hpp`, `two_opt.hpp`, `or_opt.hpp`,
  `two_opt_star.hpp`, `cross_exchange.hpp` (what they draw with is in
  `move_draw.hpp`); the sequence moves' preview, apply and undo in
  [`list_move.hpp`](../core/optimaru/search/moves/list_move.hpp).
- Settings: `config.moves` (the weights), `config.selection` (nearby, hints).
- Before writing a move, check whether reweighting these does the job
  ([First, check the built-in moves](extending.md#first-check-the-built-in-moves)).

### 9. Incremental score

Only what the move touched is scored again, which is what makes a step cheap. A
sequence move is often scored from the objective's cached data without being
applied at all (a preview), and applied only if the acceptor takes it. With
`config.run.full_assert` every incremental score is compared with a full
recompute.

- Code: `Editor` in [`core/optimaru/model/editor.hpp`](../core/optimaru/model/editor.hpp)
  (the only way a move changes the solution), the `Objective` contract in
  [`core/optimaru/score/objective.hpp`](../core/optimaru/score/objective.hpp), and
  `DecomposedObjective` with its per-unit and global constraints in
  [`score/decomposed_objective.hpp`](../core/optimaru/score/decomposed_objective.hpp).
  A use case's costs are in `optimaru/<uc>/cpp/<ns>/constraints/`.
- Settings: `config.cost` where the use case has it; the constraints themselves are
  picked and weighted per solve (`constraints=`).
- To add a cost: [Adding a cost](extending.md#adding-a-cost), and the worked
  example in each use case's `docs/use-cases/<slug>/extending.md`.

### 10. Acceptor

The acceptor decides whether the search moves to the candidate. Late acceptance by
default: a move is taken if it is no worse than the current solution or than the
one a fixed number of steps ago. The engine has seven rules: hill climbing, late
acceptance, entity tabu, simulated annealing, great deluge, step counting hill
climbing and record-to-record travel. A rejected move is undone (a previewed one
was never applied).

- Code: one file per rule in
  [`core/optimaru/accept/`](../core/optimaru/accept/), the interface
  in `acceptor.hpp` and the factory, `make_acceptor`, in `acceptors.hpp`.
- Settings: `config.acceptance` (`rule`, and each rule's own settings).
- A use case can set its own acceptor with `Solver::set_acceptor_factory`; a new
  built-in rule is a file there, a line in `acceptors.hpp`, a value of
  `AcceptanceRule` and a case in `make_acceptor`.

### 11. New best

If the current solution is better than the best, it becomes the best (the snapshot
is taken again) and the stall counters start over. Then the next step begins.

- Code: the end of the step in `Solver::run` in
  [`core/optimaru/solver.hpp`](../core/optimaru/solver.hpp).

## After the loop

The best snapshot is put back into the solution and its score returned. With
`config.run.parallel_solver_count` above 1, `Solver::solve_multistart` (in
[`core/optimaru/solver.hpp`](../core/optimaru/solver.hpp)) runs that
many solves from different seeds on threads and keeps the best. A use case that
registers moves of its own on each start goes through `solve_parallel_starts` in
[`core/optimaru/solver_config.hpp`](../core/optimaru/solver_config.hpp) instead, which
does the same around the use case's own solve.

## Where to start

- Your first contribution is most likely a move (step 7) or a cost (step 9) for one
  use case: [`CONTRIBUTING.md`](../CONTRIBUTING.md) has the setup and the short
  recipes, [`extending.md`](extending.md) the full steps with real code.
- Issues labelled
  [good first issue](https://github.com/optimaru/optimaru/labels/good%20first%20issue),
  when there are some, are small and self-contained. Otherwise propose your own with
  the "Move or constraint idea" issue template.
- A change to steps 3 to 11 in the engine must keep every use case's seeded results
  and speed: see [Changing the engine](../CONTRIBUTING.md#changing-the-engine).

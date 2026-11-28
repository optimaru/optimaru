# Container loading CP-SAT

A CP-SAT model of container loading with **free placement**
([`container_loading_cpsat/model.py`](container_loading_cpsat/model.py)): for every item
a container (or none), an orientation and a corner (x, y, z). Unlike the other
comparisons' models it lives here and not in external-solver, and it is not a plugin
catalog: it has one objective, the default constraint set.

**Constraints of the model**

- every placed item inside its container, in an orientation its `rotations` allow;
- no two items of one container overlapping;
- at least `min_support_ratio` of every base on the floor or on the tops of other items.
  The contact area of two items is the product of the lengths they share along x and y,
  as the engine counts it, and an item counts another as support only when that one's top
  is exactly at its base.

**Objective**, hard before soft, each at the default weight
`container_loading.constraint_info()` reports:

| Engine constraint | In the model |
| --- | --- |
| `unplaced_item_cost` | items left out of every container |
| `container_weight_cost` | weight of a container's items above `max_weight` |
| `used_container_cost` | the `cost` of every container that holds an item (a whole number of thousandths) |
| `group_split_cost` | per group, the containers that hold one of its items beyond the first |
| `stacking_load_cost`, `unloading_order_cost` | **not modelled**: an instance that sets `max_load_on_top` or `unload_order` is refused with `ValueError` |
| `stacking_rule_cost`, `balance_limit_cost`, `optional_item_cost`, `incomplete_group_cost`, `single_container_cost` | **not modelled**: an instance that sets `stacking_rules`, a `stack_group`, `max_stack_layers`, `bottom_only`, `max_stack_height`, a `max_offset_*`, a `priority` other than 1, `optional` or `group_rules` is refused with `ValueError` (by the evaluator too) |
| `used_length_cost`, `load_balance_cost` | weight 0 by default, so not in the objective |

Containers that are alike are used in their order, and a container's items must fit its
volume: two redundant constraints that let CP-SAT prove a bound sooner.

**What it proves.** The model's solutions include every loading the engine's decoder can
produce (the decoder's support rule with its tolerance is the model's, in whole units).
So when CP-SAT returns `OPTIMAL`, no loading order scores better. It does not prove
anything about the stacking loads or the unloading order, and it has no notion of a
loading order at all: an item it places may have no order in which the decoder would put
it there. One difference in an infeasible answer: the model leaves an item out of every
container, where the engine keeps it in some container's list, so the engine also counts
that item's weight against that container's `max_weight`.

**Size.** The contact areas need a product per pair of items, so the model is quadratic.
It is built for up to `MAX_ITEMS` (50) items; beyond that `solve` raises `RuntimeError`
with `status=TOO_LARGE`, which the comparison reports as no CP-SAT answer. In 30 s it
proves the optimum up to about 15 items.

**The evaluator.** [`container_loading_cpsat/evaluator`](container_loading_cpsat/evaluator/__init__.py)
scores a solved model from its positions alone, for both solvers: it first checks that
the positions are a loading, then computes every constraint's raw cost from them (the
two outside the model included; it refuses the inputs of the constraints in the row
above, which no instance here sets) and prices them with the engine's
`constraints.score`. `tests/test_benchmark.py` checks that it gives the engine's answers
the raw costs the engine reports, and that it refuses positions that are no loading.

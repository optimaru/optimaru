import pytest

from cpsat_plugins import (
    ConstraintPlugin,
    ConstraintRegistry,
    ConstraintSpec,
    ObjectiveBuilder,
    resolve,
)
from ortools.sat.python import cp_model

SPECS = (
    ConstraintSpec("capacity", "Capacity", "hard", 2),
    ConstraintSpec("distance", "Distance", "soft", 3),
)


def test_explicit_list_replaces_defaults() -> None:
    assert set(resolve(SPECS, None)) == {"capacity", "distance"}
    assert set(resolve(SPECS, [{"type": "distance", "weight": 7}])) == {"distance"}


def test_disabled_entry_is_omitted() -> None:
    assert resolve(SPECS, [{"name": "capacity", "enabled": False}]) == {}


def test_hard_override_moves_term_to_soft_level() -> None:
    model = cp_model.CpModel()
    value = model.new_bool_var("value")
    objective = ObjectiveBuilder(
        model, ("hard", "soft"), SPECS, [{"name": "capacity", "hard": False}]
    )
    objective.add("capacity", value, bound=1)
    objective.minimize()
    assert objective.terms["hard"] == []
    assert len(objective.terms["soft"]) == 1


def test_reward_is_reported_as_a_positive_score() -> None:
    model = cp_model.CpModel()
    reward = model.new_int_var(0, 4, "reward")
    model.add(reward == 4)
    objective = ObjectiveBuilder(
        model,
        ("hard", "soft"),
        [ConstraintSpec("reward", "Reward", "soft")],
    )
    objective.add_reward("reward", reward, bound=4)
    objective.minimize()
    solver = cp_model.CpSolver()
    assert solver.solve(model) == cp_model.OPTIMAL
    assert objective.score(solver) == {"hard": 0, "soft": 4}


def test_zero_bound_constant_is_not_added_to_score_terms() -> None:
    model = cp_model.CpModel()
    objective = ObjectiveBuilder(model, ("hard", "soft"), SPECS)
    objective.add("capacity", 0, bound=0)
    objective.minimize()
    solver = cp_model.CpSolver()
    assert solver.solve(model) == cp_model.OPTIMAL
    assert objective.terms["hard"] == []
    assert objective.score(solver) == {"hard": 0, "soft": 0}


def test_integral_float_weight_from_api_is_normalized_for_score_output() -> None:
    model = cp_model.CpModel()
    value = model.new_bool_var("value")
    model.add(value == 1)
    objective = ObjectiveBuilder(
        model,
        ("hard", "soft"),
        SPECS,
        [{"name": "capacity", "weight": 50.0}],
    )
    objective.add("capacity", value, bound=1)
    objective.minimize()
    solver = cp_model.CpSolver()
    assert solver.solve(model) == cp_model.OPTIMAL
    assert objective.score(solver) == {"hard": -50, "soft": 0}


def test_fractional_weight_is_rejected_by_integer_cp_sat_objective() -> None:
    model = cp_model.CpModel()
    value = model.new_bool_var("value")
    objective = ObjectiveBuilder(
        model,
        ("hard", "soft"),
        SPECS,
        [{"name": "capacity", "weight": 1.5}],
    )
    try:
        objective.add("capacity", value, bound=1)
    except ValueError as exc:
        assert str(exc) == "constraint 'capacity' weight must be an integer for CP-SAT"
    else:
        raise AssertionError("fractional CP-SAT weight must be rejected")


def test_canonical_costs_use_selection_levels_weights_and_per_unit_rounding() -> None:
    objective = ObjectiveBuilder(cp_model.CpModel(), ("hard", "soft"), SPECS)
    costs = {
        "capacity": {"costs": [0.25, 0.26]},
        "distance": {"costs": [4, 5]},
    }
    # hard: round(2*.25) + round(2*.26) = 1 + 1; soft: 3*(4+5).
    assert objective.score_costs(costs) == {"hard": -2, "soft": -27}


def test_canonical_cost_alias_can_sum_several_engine_components() -> None:
    objective = ObjectiveBuilder(
        cp_model.CpModel(),
        ("hard", "soft"),
        [ConstraintSpec("travel", "Travel", "soft", 2)],
    )
    costs = {"outbound": {"costs": [3]}, "return": {"costs": [7]}}
    assert objective.score_costs(costs, aliases={"travel": ("outbound", "return")}) == {
        "hard": 0,
        "soft": -20,
    }


class _DomainConstraint:
    name = "capacity"

    def __init__(self, weight=None, hard=None, exponent=1.0):
        self.weight = weight
        self.hard = hard
        self.exponent = exponent


class _CapacityPlugin(_DomainConstraint, ConstraintPlugin[dict]):
    default_level = "hard"
    default_weight = 2

    def build(self, context, objective):
        objective.add(self.name, context["value"], bound=1)


def test_registry_builds_only_selected_domain_constraint_plugins() -> None:
    model = cp_model.CpModel()
    value = model.new_bool_var("value")
    registry = ConstraintRegistry([_CapacityPlugin()])
    objective = registry.objective(
        model, ("hard", "soft"), [_DomainConstraint(weight=3)]
    )

    registry.build({"value": value}, objective)

    assert [item["name"] for item in registry.catalog()] == ["capacity"]
    assert len(objective.terms["hard"]) == 1


def test_domain_constraint_exponent_is_rejected_explicitly() -> None:
    with pytest.raises(ValueError, match="exponent is not supported"):
        resolve(SPECS, [_DomainConstraint(exponent=2.0)])

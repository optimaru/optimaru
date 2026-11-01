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
        model, ("hard", "soft"), SPECS, [{"name": "capacity", "level": "soft"}]
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

    def __init__(self, weight=None, level=None, exponent=1.0):
        self.weight = weight
        self.level = level
        self.exponent = exponent


def test_registry_builds_only_selected_domain_constraint_plugins(monkeypatch) -> None:
    spec_class = _fake_domain(monkeypatch, [("capacity", "hard", 2.0, True, False)])

    class CapacityPlugin(spec_class("capacity"), ConstraintPlugin[dict]):
        def build(self, context, objective):
            objective.add(self.name, context["value"], bound=1)

    model = cp_model.CpModel()
    value = model.new_bool_var("value")
    registry = ConstraintRegistry([CapacityPlugin()])
    objective = registry.objective(
        model, ("hard", "soft"), [_DomainConstraint(weight=3)]
    )

    registry.build({"value": value}, objective)

    assert [item["name"] for item in registry.catalog()] == ["capacity"]
    assert len(objective.terms["hard"]) == 1


def test_domain_constraint_exponent_is_rejected_explicitly() -> None:
    with pytest.raises(ValueError, match="exponent is not supported"):
        resolve(SPECS, [_DomainConstraint(exponent=2.0)])


# --- 0.2.0 knob contract ----------------------------------------------------

LEVELED = (
    ConstraintSpec("capacity", "Capacity", "hard", 1),
    ConstraintSpec("unassigned", "Unassigned", "medium", 1),
    ConstraintSpec("distance", "Distance", "soft", 1),
    ConstraintSpec("bonus", "Bonus", "soft", 1, level_flippable=False, reward=True),
    ConstraintSpec("pinned", "Pinned", "hard", 1, level_flippable=False),
)


def _level(levels, selection):
    objective = ObjectiveBuilder(cp_model.CpModel(), levels, LEVELED, [selection])
    spec, config = objective.selected[selection["name"]]
    return objective.level_of(spec, config)


@pytest.mark.parametrize(
    ("name", "level", "expected"),
    [
        ("capacity", None, "hard"),
        ("capacity", "soft", "soft"),  # not levels[1] == "medium"
        ("unassigned", None, "medium"),
        ("unassigned", "hard", "hard"),
        ("unassigned", "soft", "soft"),  # medium demotes to soft, like the engine
        ("distance", "hard", "hard"),
        ("distance", "soft", "soft"),
        ("pinned", "hard", "hard"),  # re-stating a structural level is fine
        ("bonus", "soft", "soft"),
    ],
)
def test_level_knob_maps_like_the_engine(name, level, expected) -> None:
    assert _level(("hard", "medium", "soft"), {"name": name, "level": level}) == expected


def test_hard_false_lands_medium_term_on_soft_in_scores() -> None:
    model = cp_model.CpModel()
    value = model.new_int_var(0, 3, "value")
    model.add(value == 3)
    objective = ObjectiveBuilder(
        model, ("hard", "medium", "soft"), LEVELED, [{"name": "unassigned", "level": "soft"}]
    )
    objective.add("unassigned", value, bound=3)
    objective.minimize()
    solver = cp_model.CpSolver()
    assert solver.solve(model) == cp_model.OPTIMAL
    assert objective.score(solver) == {"hard": 0, "medium": 0, "soft": -3}
    assert objective.score_costs({"unassigned": {"costs": [3]}}) == {"hard": 0, "medium": 0, "soft": -3}


def test_reward_refuses_hard_true() -> None:
    with pytest.raises(ValueError, match="reward and cannot be hard"):
        _level(("hard", "soft"), {"name": "bonus", "level": "hard"})


def test_non_flippable_constraint_refuses_a_level_change() -> None:
    with pytest.raises(ValueError, match="is always hard"):
        _level(("hard", "soft"), {"name": "pinned", "level": "soft"})


def test_hard_false_needs_a_soft_level() -> None:
    """level="soft" always means soft; a level set without one cannot take it."""
    specs = (ConstraintSpec("skill", "Skill", "hard", 1),)
    objective = ObjectiveBuilder(cp_model.CpModel(), ("hard", "medium"), specs, [{"name": "skill", "level": "soft"}])
    assert objective.level_of(*objective.selected["skill"]) == "soft"
    with pytest.raises(ValueError, match="unsupported score level 'soft'"):
        objective.add("skill", 1, bound=1)


def test_exponent_and_fractional_weight_stay_rejected() -> None:
    with pytest.raises(ValueError, match="exponent is not supported"):
        resolve(LEVELED, [_DomainConstraint(exponent=0.5)])
    objective = ObjectiveBuilder(
        cp_model.CpModel(), ("hard", "soft"), LEVELED, [{"name": "capacity", "weight": 0.5}]
    )
    with pytest.raises(ValueError, match="must be an integer"):
        objective.add("capacity", 1, bound=1)


# --- defaults from the domain's constraint_info() ---------------------------

def _fake_domain(monkeypatch, rows):
    """A domain package `fakedomain` exposing constraint_info(), and a spec class in fakedomain.constraints."""
    import collections
    import sys
    import types

    from cpsat_plugins import plugins

    fields = ["name", "default_level", "default_weight", "level_flippable", "reward"]
    if rows and len(rows[0]) == 6:
        fields.append("default_enabled")
    info = collections.namedtuple("ConstraintInfo", fields)
    package = types.ModuleType("fakedomain")
    package.constraint_info = lambda: [info(*row) for row in rows]
    monkeypatch.setitem(sys.modules, "fakedomain", package)
    monkeypatch.setattr(plugins, "_ENGINE_CATALOGS", {})

    def spec_class(name):
        return type(
            name.title().replace("_", ""),
            (_DomainConstraint,),
            {"name": name, "__module__": "fakedomain.constraints", "label_en": name.replace("_", " ")},
        )

    return spec_class


def test_migrated_plugin_reads_its_defaults_from_constraint_info(monkeypatch) -> None:
    spec_class = _fake_domain(
        monkeypatch,
        [
            ("unassigned_cost", "medium", 1.0, True, False),
            ("bonus_cost", "soft", 0.5, False, True),
        ],
    )

    class UnassignedCost(spec_class("unassigned_cost"), ConstraintPlugin[dict]):
        def build(self, context, objective):
            objective.add(self.name, context["value"], bound=1)

    class BonusCost(spec_class("bonus_cost"), ConstraintPlugin[dict]):
        def build(self, context, objective):
            objective.add_reward(self.name, context["value"], bound=1)

    registry = ConstraintRegistry([UnassignedCost(), BonusCost()])
    unassigned, bonus = registry.specs
    assert (unassigned.level, unassigned.weight, type(unassigned.weight)) == ("medium", 1, int)
    assert (bonus.level, bonus.weight, bonus.level_flippable, bonus.reward) == ("soft", 0.5, False, True)
    catalog = registry.catalog()
    assert catalog[0]["default_level"] == "medium" and catalog[1]["reward"] is True


def test_migrated_plugin_must_not_restate_engine_defaults(monkeypatch) -> None:
    spec_class = _fake_domain(monkeypatch, [("capacity_cost", "hard", 1.0, True, False)])

    class CapacityCost(spec_class("capacity_cost"), ConstraintPlugin[dict]):
        default_level = "hard"

        def build(self, context, objective):
            pass

    with pytest.raises(TypeError, match="restates an engine default"):
        CapacityCost().spec


def test_plugin_without_constraint_info_is_refused() -> None:
    """Defaults come only from the domain's constraint_info(); there is no ClassVar path."""

    class _Orphan(_DomainConstraint, ConstraintPlugin[dict]):
        default_level = "hard"
        default_weight = 2

        def build(self, context, objective):
            pass

    with pytest.raises(TypeError, match="exposes constraint_info"):
        _Orphan().spec


def test_catalog_rows_use_the_server_vocabulary() -> None:
    """kind / default_enabled / params as knob_plan §5 states them."""
    radius = ConstraintSpec("radius", "Radius", "soft", 0)
    catalog = {spec.name: spec.as_dict() for spec in (*LEVELED, radius)}
    assert {name: row["kind"] for name, row in catalog.items()} == {
        "capacity": "either",
        "unassigned": "either",
        "distance": "either",
        "bonus": "soft",  # a reward is fixed on soft
        "pinned": "hard",  # structural
        "radius": "either",
    }
    assert catalog["radius"]["default_enabled"] is False  # an opt-in weight-0 objective
    assert all(catalog[name]["default_enabled"] for name in ("capacity", "bonus"))
    # Fed back as a selection, the params change nothing (any other level would move it).
    for row in catalog.values():
        assert row["params"] == {"weight": None, "level": None, "exponent": 1.0}


def test_default_enabled_and_repeated_instances_read_the_engine_row(monkeypatch) -> None:
    """A catalog-only row (default_enabled False) and a '<name>#<k>' plugin per repeated rule."""
    spec_class = _fake_domain(
        monkeypatch,
        [
            ("required_skill_cost", "hard", 1.0, True, False, True),
            ("day_pattern_cost", "soft", 1.0, True, False, False),
        ],
    )

    class DayPatternCost(spec_class("day_pattern_cost"), ConstraintPlugin[dict]):
        def __init__(self, instance_name):
            self.name = instance_name

        def build(self, context, objective):
            pass

    class RequiredSkillCost(spec_class("required_skill_cost"), ConstraintPlugin[dict]):
        def build(self, context, objective):
            pass

    registry = ConstraintRegistry(
        [
            RequiredSkillCost(),
            DayPatternCost("day_pattern_cost"),
            DayPatternCost("day_pattern_cost#2"),
        ]
    )
    catalog = registry.catalog()
    assert [row["name"] for row in catalog] == [
        "required_skill_cost",
        "day_pattern_cost",
        "day_pattern_cost#2",
    ]
    assert [row["default_enabled"] for row in catalog] == [True, False, False]
    assert catalog[2]["default_level"] == "soft"


@pytest.mark.parametrize(
    ("selection", "error", "message"),
    [
        ({"name": "capacity", "level": "false"}, ValueError, "level must be 'hard', 'medium' or 'soft'"),
        ({"name": "capacity", "level": 1}, TypeError, "level must be a Level, its name or None"),
        ({"name": "capacity", "weight": True}, TypeError, "weight must be a number or None"),
        ({"name": "capacity", "weight": "2"}, TypeError, "weight must be a number or None"),
        ({"name": "capacity", "weight": -1}, ValueError, "weight must be finite and >= 0"),
        ({"name": "capacity", "weight": float("nan")}, ValueError, "weight must be finite and >= 0"),
        ({"name": "capacity", "exponent": 0}, ValueError, "exponent must be finite and > 0"),
        ({"name": "capacity", "exponent": 2.0}, ValueError, "exponent is not supported by CP-SAT"),
        ({"name": "capacity", "enabled": "no"}, TypeError, "enabled must be True or False"),
        (_DomainConstraint(level="false"), ValueError, "level must be 'hard', 'medium' or 'soft'"),
    ],
)
def test_mapping_selections_get_the_spec_knob_checks(selection, error, message) -> None:
    """A dict selection is checked like a spec, so the two solvers refuse the same values."""
    with pytest.raises(error, match=message):
        resolve(SPECS, [selection])


def test_a_pick_without_a_level_is_refused_rather_than_defaulted() -> None:
    class OldPick:
        name = "capacity"
        weight = None
        hard = False
        exponent = 1.0

    with pytest.raises(AttributeError, match="level"):
        resolve(SPECS, [OldPick()])


def test_server_selection_shape_and_catalog_params_round_trip() -> None:
    """{name, enabled, weight, level, exponent} with exponent 1.0 is a valid selection."""
    selected = resolve(
        SPECS,
        [
            {"name": "capacity", "enabled": True, "weight": 5, "level": None, "exponent": 1.0},
            {"name": "distance", **SPECS[1].as_dict()["params"]},
        ],
    )
    assert selected["capacity"][1].weight == 5
    assert selected["distance"][1].level is None


def test_score_costs_round_like_llround_at_the_double_edges() -> None:
    """0.49999999999999994 rounds to 0 and 2**52 + 1 stays put (C++ llround)."""
    objective = ObjectiveBuilder(
        cp_model.CpModel(),
        ("hard", "soft"),
        [ConstraintSpec("capacity", "Capacity", "hard", 1)],
    )
    assert objective.score_costs({"capacity": {"costs": [0.49999999999999994]}})["hard"] == 0
    assert objective.score_costs({"capacity": {"costs": [-0.49999999999999994]}})["hard"] == 0
    assert objective.score_costs({"capacity": {"costs": [4503599627370497.0]}})["hard"] == -4503599627370497
    assert objective.score_costs({"capacity": {"costs": [2.5]}})["hard"] == -3

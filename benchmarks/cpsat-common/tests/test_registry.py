from fractions import Fraction

import numpy as np
import pytest

from cpsat_plugins import (
    ConstraintConfig,
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

    def __init__(self, weight=None, level=None, exponent=1.0, *, offset=0.0, clip=True):
        self.weight = weight
        self.level = level
        self.exponent = exponent
        self.offset = offset
        self.clip = clip


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
        ObjectiveBuilder(cp_model.CpModel(), ("hard", "soft"), SPECS, [_DomainConstraint(exponent=2.0)])


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
    """In the engine's words (level_guard.hpp checkedLevel), as score() raises them."""
    with pytest.raises(ValueError) as refused:
        _level(("hard", "soft"), {"name": "bonus", "level": "hard"})
    assert str(refused.value) == "constraint 'bonus' is a reward (raw cost <= 0) and cannot be hard"
    # The reward refusal comes first, ahead of the literal-offset one, as in the engine.
    with pytest.raises(ValueError) as refused:
        _level(("hard", "soft"), {"name": "bonus", "level": "hard", "offset": 2, "clip": False})
    assert str(refused.value) == "constraint 'bonus' is a reward (raw cost <= 0) and cannot be hard"


def test_reward_refuses_medium_where_medium_counts_toward_hard() -> None:
    specs = (*LEVELED, ConstraintSpec("perk", "Perk", "soft", 1, reward=True))
    selection = [{"name": "perk", "level": "medium"}]
    with pytest.raises(ValueError) as refused:
        ObjectiveBuilder(cp_model.CpModel(), ("hard", "medium", "soft"), specs, selection, medium_fold="below_hard")
    assert str(refused.value) == (
        "constraint 'perk' is a reward (raw cost <= 0) and cannot be medium here, "
        "where medium counts toward the hard score"
    )
    objective = ObjectiveBuilder(
        cp_model.CpModel(), ("hard", "medium", "soft"), specs, selection, medium_fold="into_soft"
    )
    assert objective.level_of(*objective.selected["perk"]) == "medium"


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
        ObjectiveBuilder(cp_model.CpModel(), ("hard", "soft"), LEVELED, [_DomainConstraint(exponent=0.5)])
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
        assert row["params"] == {"weight": None, "level": None, "exponent": 1.0, "offset": 0.0, "clip": True}


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
        ({"name": "capacity", "offset": "1"}, TypeError, "offset must be a number, not '1'"),
        ({"name": "capacity", "offset": True}, TypeError, "offset must be a number, not True"),
        ({"name": "capacity", "offset": float("inf")}, ValueError, "offset must be finite, not inf"),
        ({"name": "capacity", "clip": 0}, TypeError, "clip must be True or False, not 0"),
        (_DomainConstraint(offset=float("nan")), ValueError, "offset must be finite, not nan"),
        (_DomainConstraint(clip="yes"), TypeError, "clip must be True or False, not 'yes'"),
    ],
)
def test_mapping_selections_get_the_spec_knob_checks(selection, error, message) -> None:
    """A dict selection is checked like a spec, so the two solvers refuse the same values."""
    with pytest.raises(error, match=message):
        ObjectiveBuilder(cp_model.CpModel(), ("hard", "soft"), SPECS, [selection])


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


# --- 0.3.0: repeated picks ---------------------------------------------------------


def test_a_name_picked_several_times_resolves_to_one_instance_per_pick() -> None:
    """Keys follow constraint_costs(): name, name#2, ... (disabled picks do not count)."""
    selected = resolve(
        SPECS,
        [
            {"name": "distance"},
            {"name": "capacity", "weight": 5},
            {"name": "distance", "enabled": False},
            {"name": "distance", "weight": 7, "offset": 3},
            "distance",
        ],
    )
    assert list(selected) == ["distance", "capacity", "distance#2", "distance#3"]
    assert selected["distance#2"][0] is SPECS[1]
    assert (selected["distance#2"][1].weight, selected["distance#2"][1].offset) == (7, 3)
    assert selected["distance#3"][1].weight is None
    # A registry with its own plugin per instance (employee-scheduling) names them already.
    specs = (*SPECS, ConstraintSpec("distance#2", "Distance", "soft", 3))
    assert list(resolve(specs, [{"name": "distance"}, {"name": "distance#2"}])) == ["distance", "distance#2"]
    with pytest.raises(ValueError, match="duplicate constraint 'distance#2'"):
        resolve(specs, [{"name": "distance"}, {"name": "distance#2"}, {"name": "distance"}])


def test_repeated_picks_add_separate_terms_with_their_own_knobs(monkeypatch) -> None:
    spec_class = _fake_domain(monkeypatch, [("capacity", "hard", 2.0, True, False)])
    built = []

    class CapacityPlugin(spec_class("capacity"), ConstraintPlugin[dict]):
        def build(self, context, objective):
            built.append(objective.selected[self.name][1])
            objective.add(self.name, context["value"], bound=4)

    model = cp_model.CpModel()
    value = model.new_int_var(0, 4, "value")
    model.add(value == 3)
    registry = ConstraintRegistry([CapacityPlugin()])
    objective = registry.objective(
        model,
        ("hard", "soft"),
        [_DomainConstraint(), _DomainConstraint(weight=5, level="soft")],
    )
    registry.build({"value": value}, objective)
    # The plugin ran once per instance and saw that instance's knobs.
    assert [(config.weight, config.level) for config in built] == [(None, None), (5, "soft")]
    assert (len(objective.terms["hard"]), len(objective.terms["soft"])) == (1, 1)
    objective.minimize()
    solver = cp_model.CpSolver()
    assert solver.solve(model) == cp_model.OPTIMAL
    assert objective.score(solver) == {"hard": -6, "soft": -15}
    costs = {"capacity": {"costs": [3]}}  # computed with the name once: the copy reads it too
    assert objective.score_costs(costs) == {"hard": -6, "soft": -15}
    held = {"capacity": {"costs": [3]}, "capacity#2": {"costs": [1]}}
    assert objective.score_costs(held) == {"hard": -6, "soft": -5}


# --- 0.3.0: offset / clip --------------------------------------------------------


def _engine_shift(value, offset, clip):
    """The optimaru base's shape() at exponent 1 (the reference CP-SAT must match)."""
    if offset == 0:
        return value
    if not clip:
        return value - offset
    if value == 0:
        return 0
    excess = abs(value) - offset
    return 0 if excess <= 0 else (-excess if value < 0 else excess)


@pytest.mark.parametrize("offset", [-3, -1, 2, 5, 0])
@pytest.mark.parametrize("clip", [True, False])
@pytest.mark.parametrize("reward", [False, True])
def test_shifted_terms_are_exact_for_every_value(offset, clip, reward) -> None:
    """threshold max(0, |x| - offset), literal x - offset: integer-exact against the engine's formula."""
    spec = ConstraintSpec("distance", "Distance", "soft", 2, reward=reward)
    for raw in range(0, 9):
        model = cp_model.CpModel()
        value = model.new_int_var(0, 8, "value")
        model.add(value == raw)
        objective = ObjectiveBuilder(
            model, ("hard", "soft"), [spec], [{"name": "distance", "offset": offset, "clip": clip}]
        )
        if reward:
            objective.add_reward("distance", value, bound=8)
        else:
            objective.add("distance", value, bound=8)
        objective.minimize()
        solver = cp_model.CpSolver()
        assert solver.solve(model) == cp_model.OPTIMAL
        x = -raw if reward else raw
        expected = 2 * _engine_shift(x, offset, clip)
        assert objective.score(solver) == {"hard": 0, "soft": -expected}, (raw, offset, clip, reward)
        assert objective.score_costs({"distance": {"costs": [x]}}) == {"hard": 0, "soft": -expected}


def test_a_literal_offset_charges_a_constant_zero_unit() -> None:
    """An evaluated unit whose raw cost is identically 0 still gets -weight * offset."""
    objective = ObjectiveBuilder(
        cp_model.CpModel(), ("hard", "soft"), SPECS, [{"name": "distance", "offset": 4, "clip": False}]
    )
    objective.add("distance", 0, bound=0)
    assert objective.terms["soft"] == [-12]
    objective.minimize()
    solver = cp_model.CpSolver()
    assert solver.solve(objective.model) == cp_model.OPTIMAL
    assert objective.score(solver) == {"hard": 0, "soft": 12}
    threshold = ObjectiveBuilder(cp_model.CpModel(), ("hard", "soft"), SPECS, [{"name": "distance", "offset": 4}])
    threshold.add("distance", 0, bound=0)
    assert threshold.terms["soft"] == []


def test_the_folded_objective_stays_lexicographic_with_negative_terms() -> None:
    """A literal term below its offset is negative; the fold uses spans, not maxima.

    The literal formula is refused on hard, so the negative term sits on an
    into_soft medium level here, above a soft reward that pulls the other way.
    """
    model = cp_model.CpModel()
    x = model.new_int_var(0, 10, "x")
    specs = (
        ConstraintSpec("capacity", "Capacity", "medium", 1),
        ConstraintSpec("bonus", "Bonus", "soft", 1000),
    )
    objective = ObjectiveBuilder(
        model,
        ("hard", "medium", "soft"),
        specs,
        [{"name": "capacity", "offset": 5, "clip": False}, "bonus"],
        medium_fold="into_soft",
    )
    objective.add("capacity", x, bound=10)  # wants x small
    objective.add_reward("bonus", x, bound=10)  # wants x large
    solver = cp_model.CpSolver()
    assert objective.solve(solver) == cp_model.OPTIMAL
    assert solver.value(x) == 0
    assert objective.score(solver) == {"hard": 0, "medium": 5, "soft": 0}


@pytest.mark.parametrize(
    ("selection", "message"),
    [
        ({"name": "capacity", "offset": 1.5}, "constraint 'capacity' offset must be an integer for CP-SAT"),
    ],
)
def test_a_fractional_offset_is_refused_like_a_fractional_weight(selection, message) -> None:
    objective = ObjectiveBuilder(cp_model.CpModel(), ("hard", "soft"), SPECS, [selection])
    with pytest.raises(ValueError, match=message):
        objective.add("capacity", 1, bound=1)
    integral = ObjectiveBuilder(cp_model.CpModel(), ("hard", "soft"), SPECS, [{"name": "capacity", "offset": 2.0}])
    integral.add("capacity", 1, bound=3)  # 2.0 is an integer


def test_score_costs_shape_each_evaluated_unit() -> None:
    objective = ObjectiveBuilder(
        cp_model.CpModel(),
        ("hard", "soft"),
        SPECS,
        [
            {"name": "capacity", "level": "soft", "offset": 1, "clip": False},
            {"name": "distance", "offset": 2},
            {"name": "capacity", "offset": 2},
        ],
    )
    costs = {
        "capacity": {"costs": [3, 0, 0, 0], "units": [0, 2]},  # scoped: units 1 and 3 never evaluated
        "distance": {"costs": [4, 1, 0, 2]},
    }
    # soft: capacity 2 x ((3 - 1) + (0 - 1)) = 2, distance 3 x (2 + 0 + 0 + 0) = 6;
    # hard: capacity#2 (threshold at 2) 2 x (1 + 0) = 2.
    assert objective.score_costs(costs) == {"hard": -2, "soft": -8}


# --- 0.3.0: clip=False with an offset stays off the hard score ---------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)
MEDIUM_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be medium here, where medium counts toward "
    "the hard score; use clip=True (a threshold) or level='soft'"
)


@pytest.mark.parametrize("offset", [3, -3])
@pytest.mark.parametrize(
    ("selection", "medium_fold", "message"),
    [
        ({"name": "capacity"}, None, HARD_REFUSAL),  # default-level hard
        ({"name": "capacity", "level": "hard"}, None, HARD_REFUSAL),
        ({"name": "distance", "level": "hard"}, "into_soft", HARD_REFUSAL),
        ({"name": "pinned"}, None, HARD_REFUSAL),  # non-flippable hard
        ({"name": "unassigned"}, "below_hard", MEDIUM_REFUSAL),  # default-level medium
        ({"name": "distance", "level": "medium"}, "below_hard", MEDIUM_REFUSAL),
    ],
)
def test_the_literal_offset_is_refused_where_it_counts_toward_hard(selection, medium_fold, message, offset) -> None:
    """Refused with the engine's words when the objective is built, before any term is added."""
    picked = {**selection, "offset": offset, "clip": False}
    with pytest.raises(ValueError) as refused:
        ObjectiveBuilder(cp_model.CpModel(), ("hard", "medium", "soft"), LEVELED, [picked], medium_fold=medium_fold)
    assert str(refused.value) == message.format(q=repr(selection["name"]))


@pytest.mark.parametrize(
    "selection",
    [
        {"name": "capacity", "offset": 3},  # clip=True on hard: a threshold
        {"name": "capacity", "offset": -3},
        {"name": "capacity", "clip": False},  # clip=False with offset 0
        {"name": "capacity", "offset": 0, "clip": False},
        {"name": "capacity", "level": "soft", "offset": 3, "clip": False},
        {"name": "distance", "offset": -3, "clip": False},
        {"name": "unassigned", "offset": 3, "clip": False},  # medium into soft
        {"name": "bonus", "offset": 2, "clip": False},  # a soft reward
    ],
)
def test_the_allowed_offset_combinations(selection) -> None:
    objective = ObjectiveBuilder(
        cp_model.CpModel(), ("hard", "medium", "soft"), LEVELED, [selection], medium_fold="into_soft"
    )
    spec, config = objective.selected[selection["name"]]
    assert objective.level_of(spec, config) in ("hard", "medium", "soft")


def test_the_literal_offset_refusal_reaches_every_entry_path(monkeypatch) -> None:
    """The registry (any plugin), a duck-typed pick, a repeated instance and score_costs."""
    spec_class = _fake_domain(monkeypatch, [("capacity", "hard", 1.0, True, False)])

    class SummedPlugin(spec_class("capacity"), ConstraintPlugin[dict]):
        def build(self, context, objective):
            objective.add(self.name, context, bound=1)

    message = HARD_REFUSAL.format(q="'capacity'")
    registry = ConstraintRegistry([SummedPlugin()])
    for picks in (
        [_DomainConstraint(offset=2.0, clip=False)],
        [_DomainConstraint(), _DomainConstraint(offset=-2.0, clip=False)],
        [{"name": "capacity", "level": "hard", "offset": 2, "clip": False}],
    ):
        with pytest.raises(ValueError) as refused:
            registry.objective(cp_model.CpModel(), ("hard", "soft"), picks)
        assert str(refused.value) == message
    # Moved to soft, the same pick passes the level rule (and then needs a per-unit plugin).
    with pytest.raises(ValueError, match="offset is not supported by this CP-SAT model"):
        registry.objective(cp_model.CpModel(), ("hard", "soft"), [_DomainConstraint(level="soft", offset=2.0, clip=False)])
    # level_of refuses it too, so score_costs on a hand-edited selection cannot score it.
    objective = ObjectiveBuilder(cp_model.CpModel(), ("hard", "soft"), SPECS, [{"name": "capacity"}])
    spec, _config = objective.selected["capacity"]
    objective.selected["capacity"] = (spec, ConstraintConfig(name="capacity", offset=1, clip=False))
    with pytest.raises(ValueError, match="cannot be hard"):
        objective.score_costs({"capacity": {"costs": [0]}})


# --- the engine's rules before CP-SAT's own, on the doubles the engine reads -----------


@pytest.mark.parametrize(
    "offset", [Fraction(1, 10**400), np.longdouble("1e-4000")], ids=["Fraction", "longdouble"]
)
def test_an_offset_that_is_zero_as_a_double_is_no_offset(offset) -> None:
    """The engine reads float(offset) and sees 0.0, so a literal pick on hard is allowed there."""
    for selection in (
        _DomainConstraint(offset=offset, clip=False),
        {"name": "capacity", "offset": offset, "clip": False},
    ):
        objective = ObjectiveBuilder(cp_model.CpModel(), ("hard", "soft"), SPECS, [selection])
        _spec, config = objective.selected["capacity"]
        assert config.offset == 0.0 and type(config.offset) is float
        objective.add("capacity", 1, bound=1)
        assert len(objective.terms["hard"]) == 1


def test_numeric_knobs_are_the_engine_doubles() -> None:
    config = ConstraintConfig.parse(
        {"name": "capacity", "weight": np.int64(3), "offset": np.float32(2), "exponent": Fraction(1)}
    )
    assert (config.weight, config.offset, config.exponent) == (3.0, 2.0, 1.0)
    assert all(type(value) is float for value in (config.weight, config.offset, config.exponent))


@pytest.mark.parametrize(
    ("selection", "medium_fold", "message"),
    [
        ({"name": "capacity", "exponent": 2, "offset": 2, "clip": False}, None, HARD_REFUSAL),
        (_DomainConstraint(exponent=2.0, offset=-1.0, clip=False), None, HARD_REFUSAL),
        ({"name": "unassigned", "exponent": 2, "offset": 2, "clip": False}, "below_hard", MEDIUM_REFUSAL),
    ],
)
def test_the_engine_rules_come_before_the_exponent_limit(selection, medium_fold, message) -> None:
    """The engine refuses the literal pick; CP-SAT's exponent limit is only asked after that."""
    with pytest.raises(ValueError) as refused:
        ObjectiveBuilder(cp_model.CpModel(), ("hard", "medium", "soft"), LEVELED, [selection], medium_fold=medium_fold)
    name = selection["name"] if isinstance(selection, dict) else selection.name
    assert str(refused.value) == message.format(q=repr(name))


def test_across_picks_the_engine_rules_come_first() -> None:
    """[A(exponent=2), B(literal on hard)]: the engine refuses B, so CP-SAT does too."""
    picks = [
        {"name": "distance", "exponent": 2},
        {"name": "capacity", "offset": 1, "clip": False},
    ]
    with pytest.raises(ValueError) as refused:
        ObjectiveBuilder(cp_model.CpModel(), ("hard", "medium", "soft"), LEVELED, picks)
    assert str(refused.value) == HARD_REFUSAL.format(q="'capacity'")
    # Without B, A's exponent is what CP-SAT refuses.
    with pytest.raises(ValueError, match="constraint 'distance' exponent is not supported by CP-SAT"):
        ObjectiveBuilder(cp_model.CpModel(), ("hard", "medium", "soft"), LEVELED, picks[:1])


def test_a_per_pick_plugin_is_named_by_its_constraint_in_the_engine_messages() -> None:
    """A registry with a plugin per pick names the k-th one '<name>#<k>' (employee scheduling);
    the engine's refusals still name the constraint."""
    specs = (*LEVELED, ConstraintSpec("bonus#2", "Bonus", "soft", 1, level_flippable=False, reward=True))
    for selection, message in (
        ({"name": "bonus#2", "level": "hard"}, "constraint 'bonus' is a reward (raw cost <= 0) and cannot be hard"),
        ({"name": "bonus#2", "level": "medium"}, "constraint 'bonus' is always soft; level='medium' is not allowed"),
        ({"name": "bonus#2", "offset": 1, "clip": False}, None),  # soft: allowed
    ):
        if message is None:
            ObjectiveBuilder(cp_model.CpModel(), ("hard", "medium", "soft"), specs, [selection])
            continue
        with pytest.raises(ValueError) as refused:
            ObjectiveBuilder(cp_model.CpModel(), ("hard", "medium", "soft"), specs, [selection])
        assert str(refused.value) == message
    specs = (*SPECS, ConstraintSpec("capacity#2", "Capacity", "hard", 2))
    with pytest.raises(ValueError) as refused:
        ObjectiveBuilder(cp_model.CpModel(), ("hard", "soft"), specs, [{"name": "capacity#2", "offset": 1, "clip": False}])
    assert str(refused.value) == HARD_REFUSAL.format(q="'capacity'")


def test_refuse_signed_is_the_engine_sign_rule() -> None:
    """A plugin whose data can make its raw cost negative refuses a level that counts toward hard."""
    head = "constraint {q} can have a negative raw cost on this problem (a bonus) and cannot be "
    levels = ("hard", "medium", "soft")
    for selection, fold, message in (
        ({"name": "distance", "level": "hard"}, None, head.format(q="'distance'") + "hard"),
        ({"name": "capacity"}, None, head.format(q="'capacity'") + "hard"),  # its default level
        (
            {"name": "unassigned"},
            "below_hard",
            head.format(q="'unassigned'") + "medium here, where medium counts toward the hard score",
        ),
    ):
        objective = ObjectiveBuilder(cp_model.CpModel(), levels, LEVELED, [selection], medium_fold=fold)
        with pytest.raises(ValueError) as refused:
            objective.refuse_signed(selection["name"])
        assert str(refused.value) == message
    for selection, fold in (({"name": "distance"}, None), ({"name": "unassigned"}, "into_soft")):
        ObjectiveBuilder(cp_model.CpModel(), levels, LEVELED, [selection], medium_fold=fold).refuse_signed(
            selection["name"]
        )
    # A repeat is checked under its own key, as the plugin of that instance sees it.
    objective = ObjectiveBuilder(
        cp_model.CpModel(), levels, LEVELED, [{"name": "distance"}, {"name": "distance", "level": "hard"}]
    )
    objective.refuse_signed("distance")
    with pytest.raises(ValueError, match="'distance' can have a negative raw cost"):
        objective.refuse_signed("distance#2")
    from cpsat_plugins.plugins import InstanceObjective

    with pytest.raises(ValueError, match="'distance' can have a negative raw cost"):
        InstanceObjective(objective, "distance#2", "distance").refuse_signed("distance")


def test_medium_fold_is_validated() -> None:
    with pytest.raises(ValueError, match="medium_fold must be None, 'into_soft' or 'below_hard'"):
        ObjectiveBuilder(cp_model.CpModel(), ("hard", "soft"), SPECS, medium_fold="below")


def test_an_offset_needs_a_plugin_with_per_unit_terms(monkeypatch) -> None:
    spec_class = _fake_domain(monkeypatch, [("capacity", "hard", 1.0, True, False)])

    class SummedPlugin(spec_class("capacity"), ConstraintPlugin[dict]):
        def build(self, context, objective):
            objective.add(self.name, sum(context["values"]), bound=8)

    class PerUnitPlugin(spec_class("capacity"), ConstraintPlugin[dict]):
        terms_per_unit = True

        def build(self, context, objective):
            for value in context["values"]:
                objective.add(self.name, value, bound=4)

    model = cp_model.CpModel()
    values = [model.new_int_var(0, 4, f"v{index}") for index in range(2)]
    model.add(values[0] == 3)
    model.add(values[1] == 1)
    with pytest.raises(ValueError, match="constraint 'capacity' offset is not supported by this CP-SAT model"):
        ConstraintRegistry([SummedPlugin()]).objective(model, ("hard", "soft"), [_DomainConstraint(offset=2.0)])
    registry = ConstraintRegistry([PerUnitPlugin()])
    objective = registry.objective(model, ("hard", "soft"), [_DomainConstraint(), _DomainConstraint(offset=2.0)])
    registry.build({"values": values}, objective)
    solver = cp_model.CpSolver()
    assert objective.solve(solver) == cp_model.OPTIMAL
    # capacity: 3 + 1; capacity#2 (threshold at 2): 1 + 0.
    assert objective.score(solver) == {"hard": -5, "soft": 0}

"""The canonical model and its engine conversion (optimaru.bin_packing.models)."""

import json
from pathlib import Path

import pytest
from pydantic import ValidationError

import optimaru.bin_packing as bp
from optimaru.bin_packing.constraints import get_constraints
from optimaru.bin_packing.models import BinPackingProblemModel, ConflictModel, ItemModel

WEB_DEMO = Path(__file__).parent / "fixtures" / "web_demo.json"


def rich_model() -> BinPackingProblemModel:
    return BinPackingProblemModel(
        bin_capacity=10,
        items=[
            ItemModel(id="A", weight=8, bin=0, color="red"),
            ItemModel(id="B", weight=7, bin=2),
            ItemModel(id="C", weight=3, color="red"),
            ItemModel(id="D", weight=2, bin=2),
        ],
        conflicts=[ConflictModel(first="A", second="C"), ConflictModel(first="C", second="A")],
        warm_start=True,
    )


def test_to_core_keeps_every_fact():
    core = rich_model().to_core()
    assert core.bin_capacity == 10 and core.warm_start is True
    assert [(i.id, i.weight, i.bin, i.color) for i in core.items] == [
        ("A", 8, 0, "red"),
        ("B", 7, 2, None),
        ("C", 3, -1, "red"),
        ("D", 2, 2, None),
    ]
    assert core.conflicts == [("A", "C"), ("C", "A")]  # duplicates kept: each listing counts
    assert core.assignment() == [0, 2, -1, 2]


def test_round_trip_through_the_core_is_lossless():
    model = rich_model()
    back = BinPackingProblemModel.from_core(model.to_core())
    outputs = {"bins_used", "bin_loads", "score_hard", "score_soft"}
    assert back.model_dump(exclude=outputs) == model.model_dump(exclude=outputs)
    assert (back.bins_used, back.bin_loads) == (2, [8, 0, 9])  # gaps show as 0
    clone = BinPackingProblemModel.model_validate_json(back.model_dump_json())
    assert clone == back


def test_from_core_reports_the_solved_outputs():
    core = BinPackingProblemModel.model_validate(json.loads(WEB_DEMO.read_text())).to_core()
    bp.solve(core, time_limit_seconds=0.5, random_seed=7)
    solved = BinPackingProblemModel.from_core(core)
    assert (solved.score_hard, solved.score_soft) == (core.score.hard, core.score.soft)
    assert solved.bins_used == core.bins_used() == len(solved.bin_loads)
    assert sum(solved.bin_loads) == sum(item.weight for item in solved.items)
    assert solved.warm_start is False


@pytest.mark.parametrize("field", ["capacity_weight", "used_bin_weight", "color_weight", "conflict_weight"])
def test_weight_fields_are_refused(field):
    """The canonical problem carries no weights; extra="forbid" refuses weight fields."""
    data = BinPackingProblemModel.from_core(rich_model().to_core()).model_dump()
    assert field not in data
    data[field] = 2
    with pytest.raises(ValidationError):
        BinPackingProblemModel.model_validate(data)
    assert not hasattr(bp.BinPackingProblem(10, []), field)


@pytest.mark.parametrize(
    ("change", "message"),
    [
        ({"items": [{"id": "A", "weight": 1}, {"id": "A", "weight": 2}]}, "item ids must be unique"),
        ({"conflicts": [{"first": "A", "second": "Z"}]}, "conflict endpoints"),
        ({"conflicts": [{"first": "A", "second": "A"}]}, "conflict endpoints"),
        ({"items": [{"id": "A", "weight": 0}]}, "greater than 0"),
        ({"bin_capacity": 0}, "greater than 0"),
        ({"items": [{"id": "A", "weight": 1, "bin": -2}]}, "greater than or equal to -1"),
    ],
)
def test_the_model_validates_the_facts(change, message):
    data = {"bin_capacity": 10, "items": [{"id": "A", "weight": 1}, {"id": "B", "weight": 2}], **change}
    with pytest.raises(ValidationError, match=message):
        BinPackingProblemModel.model_validate(data)


def test_the_core_checks_the_facts_the_model_cannot_see():
    """Core-only construction: the C++ prepare() refuses what would index past the per-bin data."""
    problem = bp.BinPackingProblem(10, [bp.Item("A", 3, 5), bp.Item("B", 4)])
    with pytest.raises(ValueError, match="item 'A' has bin 5, outside -1..1"):
        bp.calculate_score(problem)
    with pytest.raises(ValueError, match="outside -1..1"):
        bp.constraint_costs(problem)
    with pytest.raises(ValueError, match="item ids must be unique"):
        bp.calculate_score(bp.BinPackingProblem(10, [bp.Item("A", 3), bp.Item("A", 4)]))
    with pytest.raises(ValueError, match="item weights must be positive"):
        bp.calculate_score(bp.BinPackingProblem(10, [bp.Item("A", 0)]))
    with pytest.raises(ValueError, match="conflict endpoints must be two different item ids"):
        bp.calculate_score(bp.BinPackingProblem(10, [bp.Item("A", 3)], conflicts=[("A", "Z")]))
    with pytest.raises(ValueError, match="bin_capacity must be positive"):
        bp.solve(bp.BinPackingProblem(0, [bp.Item("A", 3)]), time_limit_seconds=0.0)


@pytest.mark.parametrize("capacity", [0, -3])
def test_every_scoring_entry_point_refuses_a_non_positive_capacity(capacity):
    """Core-only construction: scoring checks bin_capacity too, not only solve().

    Without the check a negative capacity would score -7hard through the monolith and
    -13hard through the plugin assembly (which also charges the empty bins); both raise.
    """
    problem = bp.BinPackingProblem(capacity, [bp.Item("A", 2, 0), bp.Item("B", 2, 0), bp.Item("C", 1)])
    with pytest.raises(ValueError, match="bin_capacity must be positive"):
        bp.calculate_score(problem)
    with pytest.raises(ValueError, match="bin_capacity must be positive"):
        bp.calculate_score(problem, get_constraints())
    with pytest.raises(ValueError, match="bin_capacity must be positive"):
        bp.constraint_costs(problem)


def test_a_capacity_edited_after_scoring_is_refused():
    problem = bp.BinPackingProblem(10, [bp.Item("A", 8, 0), bp.Item("B", 7, 0)])
    assert bp.calculate_score(problem) == bp.Score(-5, -1.0)
    problem.bin_capacity = -1  # the derived lookups are already built
    with pytest.raises(ValueError, match="bin_capacity must be positive"):
        bp.calculate_score(problem)
    with pytest.raises(ValueError, match="bin_capacity must be positive"):
        bp.constraint_costs(problem)


def test_a_bin_beyond_the_items_is_accepted_by_the_model_and_ignored_by_a_plain_solve():
    """The model only bounds bin >= -1; a plain solve ignores the input bins anyway."""
    model = BinPackingProblemModel(bin_capacity=10, items=[ItemModel(id="A", weight=3, bin=9)])
    core = model.to_core()
    bp.solve(core, time_limit_seconds=0.0)
    assert core.assignment() == [0]


def test_items_are_copies_and_replacing_the_lists_is_picked_up():
    problem = bp.BinPackingProblem(10, [bp.Item("A", 8), bp.Item("B", 7)])
    problem.items[0].bin = 0  # a copy: lost
    assert problem.assignment() == [-1, -1]
    problem.set_assignment([0, 0])
    assert bp.calculate_score(problem) == bp.Score(-5, -1.0)
    problem.items = [bp.Item("A", 8, 0, "red"), bp.Item("B", 1, 0, "red")]
    assert bp.calculate_score(problem) == bp.Score(-1, -1.0)
    problem.conflicts = [("A", "B")]
    assert bp.calculate_score(problem) == bp.Score(-2, -1.0)
    with pytest.raises(ValueError, match="assignment length"):
        problem.set_assignment([0])


def test_score_and_item_are_plain_values():
    assert bp.Score() == bp.Score(0, 0.0)
    assert bp.Score(0, -2.0) > bp.Score(-1, 0.0) >= bp.Score(-1, -5.0)
    assert len({bp.Score(0, -2.0), bp.Score(0, -2.0)}) == 1
    assert str(bp.Score(-1, -2.5)) == "-1hard/-2.5soft"
    assert repr(bp.Item("A", 3, 1, "red")) == "Item(id='A', weight=3, bin=1, color='red')"


def _steps(steps):
    config = bp.SolverConfig()
    config.step_count_limit = steps
    return config


def test_solve_takes_the_model_and_returns_a_solved_model():
    model = rich_model()
    solved = bp.solve(model, config=_steps(500), random_seed=3)
    assert isinstance(solved, BinPackingProblemModel)
    core = model.to_core()
    bp.solve(core, config=_steps(500), random_seed=3)
    assert (solved.score_hard, solved.score_soft) == (core.score.hard, core.score.soft)


def test_solve_refuses_anything_but_the_problem_or_its_model():
    with pytest.raises(TypeError, match="must be a"):
        bp.solve(3)

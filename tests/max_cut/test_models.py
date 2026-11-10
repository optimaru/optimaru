"""optimaru.max_cut.models — the canonical JSON contract and its conversion to the engine object."""

import json
from pathlib import Path

import pytest
from pydantic import ValidationError

from optimaru import max_cut
from optimaru.max_cut.models import EdgeModel, MaxCutProblemModel, VertexPairModel

FIXTURES = Path(__file__).parent / "fixtures"


def rich() -> MaxCutProblemModel:
    """Every fact, with repeats, a self-loop, a self-pair and a fixed_partition dict order that is not sorted."""
    return MaxCutProblemModel(
        vertices=["A", "B", "C", "D"],
        edges=[
            EdgeModel(source="A", target="B", weight=3),
            EdgeModel(source="B", target="A", weight=1),
            EdgeModel(source="C", target="C", weight=5),
            EdgeModel(source="C", target="D"),
            EdgeModel(source="B", target="D", weight=0),
        ],
        fixed_partition={"D": 1, "A": 0},
        same_partition_pairs=[VertexPairModel(first="A", second="C"), VertexPairModel(first="A", second="C")],
        different_partition_pairs=[VertexPairModel(first="B", second="B"), VertexPairModel(first="C", second="D")],
        min_partition_size=1,
        partition={"A": 0, "B": 1, "C": 0, "D": 1},
    )


def test_to_core_maps_ids_to_indices_and_keeps_order_and_repeats():
    core = rich().to_core()
    assert isinstance(core, max_cut.MaxCutProblem)
    assert core.vertex_ids == ["A", "B", "C", "D"]
    assert [(edge.source, edge.target, edge.weight) for edge in core.edges] == [
        (0, 1, 3),
        (1, 0, 1),
        (2, 2, 5),
        (2, 3, 1),
        (1, 3, 0),
    ]
    assert core.fixed_side == [0, -1, -1, 1]
    assert [(pair.first, pair.second) for pair in core.same_partition_pairs] == [(0, 2), (0, 2)]
    assert [(pair.first, pair.second) for pair in core.different_partition_pairs] == [(1, 1), (2, 3)]
    assert (core.min_partition_size, core.max_partition_size) == (1, None)
    assert core.assignment() == [0, 1, 0, 1]
    assert core.warm_start is False
    assert core.scored is False


def test_a_partial_partition_becomes_minus_one_on_the_core():
    model = rich()
    model.partition = {"B": 0}
    assert model.to_core().assignment() == [-1, 0, -1, -1]
    assert not model.is_complete() and not model.to_core().is_complete()


def test_from_core_round_trips_the_facts_and_fills_the_results_once_scored():
    model = rich()
    core = model.to_core()
    unscored = MaxCutProblemModel.from_core(core)
    assert unscored.model_dump() == model.model_dump()  # results stay None until scored
    max_cut.calculate_score(core)
    back = MaxCutProblemModel.from_core(core)
    expected = model.model_dump()
    # A-B 3 and B-A 1 cut (the self-loop C-C never is, C-D 1 is, B-D weighs 0).
    expected.update(cut_weight=5, score_hard=core.score.hard, score_soft=core.score.soft)
    assert back.model_dump() == expected
    assert back.score_soft == 5.0
    assert list(back.fixed_partition) == ["A", "D"]  # vertex order, not the dict order


def test_from_core_leaves_unassigned_vertices_out_of_the_partition():
    core = rich().to_core()
    core.set_assignment([1, -1, 0, -1])
    assert MaxCutProblemModel.from_core(core).partition == {"A": 1, "C": 0}


def test_from_core_of_a_solved_core_fills_the_results():
    core = rich().to_core()
    max_cut.solve(core, time_limit_seconds=0.2, random_seed=1)
    back = MaxCutProblemModel.from_core(core)
    assert set(back.partition) == set(back.vertices)
    assert back.cut_weight == core.cut_weight()
    assert (back.score_hard, back.score_soft) == (core.score.hard, core.score.soft)
    assert back.warm_start is False


def test_the_web_demo_validates_and_converts():
    data = json.loads((FIXTURES / "web_demo.json").read_text())
    model = MaxCutProblemModel.model_validate(data)
    core = model.to_core()
    assert core.vertex_count() == 8 and core.is_exact_size()
    assert core.fixed_side == [0, -1, -1, -1, -1, -1, -1, -1]
    assert core.assignment() == [-1] * 8  # the sample carries no partition
    assert MaxCutProblemModel.from_core(core).model_dump() == model.model_dump()


def test_solving_the_model_returns_a_new_solved_model_and_leaves_the_input_alone():
    problem = rich()
    problem.warm_start = True
    before = problem.model_dump()
    model = max_cut.solve(problem, time_limit_seconds=0.2, random_seed=3)
    assert isinstance(model, MaxCutProblemModel) and model is not problem
    assert problem.model_dump() == before
    result = max_cut.calculate_score(model.model_copy(deep=True))
    assert (model.score_hard, model.score_soft) == (result.hard, result.soft)
    assert model.warm_start is False
    assert model.cut_weight == result.soft  # the default score's soft level is the cut weight


def test_the_json_schema_fields():
    """The OpenAPI components and the web sample depend on this field set."""
    schema = MaxCutProblemModel.model_json_schema()
    assert set(schema["properties"]) == {
        "vertices",
        "edges",
        "fixed_partition",
        "same_partition_pairs",
        "different_partition_pairs",
        "min_partition_size",
        "max_partition_size",
        "partition",
        "cut_weight",
        "score_hard",
        "score_soft",
        "warm_start",
    }
    assert schema["required"] == ["vertices", "edges"]
    assert set(schema["$defs"]) == {"EdgeModel", "VertexPairModel"}
    assert set(schema["$defs"]["EdgeModel"]["properties"]) == {"source", "target", "weight"}
    assert schema["properties"]["partition"]["description"] == "Solution side, 0 or 1, for each vertex."


@pytest.mark.parametrize(
    "change, message",
    [
        (lambda d: d["vertices"].append("A"), "unique"),
        (lambda d: d["edges"].append({"source": "A", "target": "Z"}), "edge endpoint"),
        (lambda d: d["same_partition_pairs"].append({"first": "A", "second": "Z"}), "unknown vertex references"),
        (lambda d: d["fixed_partition"].update(Z=0), "unknown vertex references"),
        (lambda d: d["fixed_partition"].update(A=2), "fixed partition sides"),
        (lambda d: d.update(min_partition_size=3, max_partition_size=2), "bounds"),
        (lambda d: d.update(max_partition_size=5), "bounds"),
        (lambda d: d["edges"][0].update(weight=-1), "greater than or equal to 0"),
        (lambda d: d.update(min_partition_size=-1), "greater than or equal to 0"),
        (lambda d: d.update(partition_weight=1), "Extra inputs"),
    ],
)
def test_validators_are_unchanged(change, message):
    data = rich().model_dump()
    change(data)
    with pytest.raises(ValidationError, match=message):
        MaxCutProblemModel.model_validate(data)


@pytest.mark.parametrize(
    "partition, message",
    [
        ({"A": 0, "Z": 1}, "partition references unknown vertices: \\['Z'\\]"),
        ({"A": 2}, "partition sides must be 0 or 1"),
        ({"A": -1}, "partition sides must be 0 or 1"),
    ],
)
def test_the_partition_is_validated(partition, message):
    """The engine keeps one side per vertex, so stray entries are refused."""
    data = rich().model_dump()
    data["partition"] = partition
    with pytest.raises(ValidationError, match=message):
        MaxCutProblemModel.model_validate(data)
    model = rich()
    model.partition = partition  # a field edited after validation is checked again by to_core()
    with pytest.raises(ValueError, match=message):
        model.to_core()


def test_the_core_validates_what_a_direct_caller_could_get_wrong():
    """A core built by hand is checked by the C++ prepare(), as ValueError."""
    core = rich().to_core()
    core.edges = [max_cut.Edge(0, 7)]
    with pytest.raises(ValueError, match="out of range"):
        max_cut.calculate_score(core)
    core = rich().to_core()
    core.edges = [max_cut.Edge(0, 1, -2)]
    with pytest.raises(ValueError, match="weight must be >= 0"):
        max_cut.constraint_costs(core)
    core = rich().to_core()
    core.same_partition_pairs = [max_cut.VertexPair(0, 9)]
    with pytest.raises(ValueError, match="out of range"):
        max_cut.solve(core, time_limit_seconds=0)
    core = rich().to_core()
    core.max_partition_size = 9
    with pytest.raises(ValueError, match="size bounds"):
        max_cut.calculate_score(core)
    core = rich().to_core()
    core.vertex_ids = ["A", "A", "B", "C"]
    with pytest.raises(ValueError, match="more than once"):
        max_cut.calculate_score(core)
    core = rich().to_core()
    with pytest.raises(ValueError, match="-1, 0 or 1"):
        core.set_assignment([0, 1, 2, 0])
    with pytest.raises(ValueError, match="length"):
        core.set_assignment([0, 1])
    with pytest.raises(ValueError, match="fixed_side length"):
        max_cut.MaxCutProblem(["A", "B"], fixed_side=[0])
    with pytest.raises(ValueError, match="side must be 0 or 1"):
        core.side_size(2)

"""The pydantic contract: JSON in, a solved model out, and back into the engine."""

import json
from pathlib import Path

import pytest
from pydantic import ValidationError

import optimaru.container_loading as cl
from optimaru.container_loading.models import (
    ContainerLoadingProblemModel,
    ContainerLoadingRequest,
    ContainerModel,
    ItemModel,
    solve_request,
)

FIXTURES = Path(__file__).parent / "fixtures"


def van() -> dict:
    return json.loads((FIXTURES / "van_route.json").read_text())


def small() -> ContainerLoadingProblemModel:
    return ContainerLoadingProblemModel(
        containers=[
            {"id": "c1", "length": 10, "width": 10, "height": 10},
            {"id": "c2", "length": 10, "width": 10, "height": 10},
        ],
        items=[{"id": f"i{i}", "length": 5, "width": 5, "height": 5, "rotations": "fixed"} for i in range(9)],
    )


def test_the_field_names_are_the_contract():
    """The WMS backend sends these names: a rename here is a breaking change of the API."""
    assert list(ContainerLoadingRequest.model_fields)[-1] == "model_input"
    assert list(ContainerLoadingProblemModel.model_fields) == [
        "containers",
        "items",
        "min_support_ratio",
        "stacking_rules",
        "score_hard",
        "score_soft",
        "containers_used",
        "unplaced_items",
    ]
    assert list(ContainerModel.model_fields) == [
        "id",
        "length",
        "width",
        "height",
        "max_weight",
        "cost",
        "max_stack_height",
        "max_offset_length",
        "max_offset_width",
        "initial_items",
        "items",
        "unplaced_items",
        "loaded_weight",
        "loaded_volume",
        "fill_ratio",
        "used_length",
    ]
    assert list(ItemModel.model_fields) == [
        "id",
        "length",
        "width",
        "height",
        "weight",
        "rotations",
        "max_load_on_top",
        "unload_order",
        "group",
        "bottom_only",
        "max_stack_layers",
        "stack_group",
        "container",
        "sequence",
        "x",
        "y",
        "z",
        "placed_length",
        "placed_width",
        "placed_height",
    ]


def test_the_defaults():
    model = small()
    assert model.min_support_ratio == 0.8
    container, item = model.containers[0], ContainerLoadingProblemModel.model_validate(van()).items[1]
    assert (container.max_weight, container.cost, container.initial_items, container.items) == (None, 1.0, [], None)
    assert (item.rotations, item.max_load_on_top, item.weight) == ("all", None, 15000)
    assert ItemModel(id="a", length=1, width=1, height=1).weight == 0
    bare = ItemModel(id="a", length=1, width=1, height=1)
    assert (bare.bottom_only, bare.max_stack_layers, bare.stack_group) == (False, None, None)
    assert (container.max_stack_height, container.max_offset_length, container.max_offset_width) == (None, None, None)
    assert model.stacking_rules == []
    assert (item.container, item.sequence, item.x, item.placed_length) == (None, None, None, None)


def test_a_solve_fills_in_the_outputs_and_leaves_the_input_alone():
    model = small()
    before = model.model_dump()
    solved = cl.solve(model, time_limit_seconds=0.3, random_seed=1)
    assert model.model_dump() == before
    assert isinstance(solved, ContainerLoadingProblemModel) and solved is not model
    assert (solved.score_hard, solved.score_soft, solved.containers_used) == (0, -2.0, 2)
    assert solved.unplaced_items == []
    assert sorted(len(container.items) for container in solved.containers) == [1, 8]
    for container in solved.containers:
        assert container.unplaced_items == []
        assert container.loaded_volume == 125 * len(container.items)
        assert container.fill_ratio == pytest.approx(len(container.items) / 8)
        assert container.used_length == (10 if len(container.items) == 8 else 5)
        assert container.loaded_weight == 0
        for sequence, item_id in enumerate(container.items):
            item = next(item for item in solved.items if item.id == item_id)
            assert (item.container, item.sequence) == (container.id, sequence)
            assert (item.placed_length, item.placed_width, item.placed_height) == (5, 5, 5)
            assert {item.x, item.y, item.z} <= {0, 5}
    assert json.loads(solved.model_dump_json()) == solved.model_dump(mode="json")
    assert ContainerLoadingProblemModel.model_validate_json(solved.model_dump_json()) == solved


def test_an_item_without_a_place_has_no_position():
    model = ContainerLoadingProblemModel(
        containers=[{"id": "c1", "length": 10, "width": 10, "height": 10, "max_weight": 5}],
        items=[
            {"id": "big", "length": 11, "width": 5, "height": 5, "weight": 4},
            {"id": "ok", "length": 5, "width": 5, "height": 5, "weight": 3},
        ],
    )
    solved = cl.solve(model, time_limit_seconds=0.2)
    big = solved.items[0]
    assert (big.container, big.sequence, big.x, big.y, big.z) == (None, None, None, None, None)
    assert (big.placed_length, big.placed_width, big.placed_height) == (None, None, None)
    assert solved.unplaced_items == ["big"]
    container = solved.containers[0]
    assert (container.items, container.unplaced_items) == (["ok"], ["big"])
    # One item without a place, and the container's list weighs 7 against a limit of 5.
    assert (solved.score_hard, solved.score_soft, solved.containers_used) == (-3, -1.0, 1)
    assert container.loaded_weight == 3


def test_a_solved_model_scores_as_solved_and_solves_on():
    """to_core() loads what a solve filled in (items, and the unplaced ones after them), so a
    solved model is the same solution in the engine: same score, same positions."""
    model = ContainerLoadingProblemModel.model_validate(van())
    model.items.append(ItemModel(id="too-long", length=3500, width=100, height=100, weight=1000))
    solved = cl.solve(model, time_limit_seconds=1.0, random_seed=0)
    assert solved.score_hard == -1 and solved.unplaced_items == ["too-long"]
    core = solved.to_core()
    score = cl.calculate_score(core)
    assert (score.hard, score.soft) == (solved.score_hard, solved.score_soft)
    again = ContainerLoadingProblemModel.from_core(core)
    assert again.model_dump() == solved.model_copy(deep=True).model_dump()
    config = cl.SolverConfig(stop={"time_limit_seconds": 0})
    resumed = cl.solve(solved, config=config)
    assert [(item.container, item.x, item.y, item.z) for item in resumed.items] == [
        (item.container, item.x, item.y, item.z) for item in solved.items
    ]


def test_a_round_trip_never_scores_worse_than_the_solve():
    """Everything placed: the same score exactly. With unplaced items, which are loaded back
    after the placed ones, never more of them (the hard score may still differ: an item placed
    now may break a limit or move a centre of gravity, as `loading_order()` says)."""
    from .test_reference_parity import random_problem

    for seed in range(20):
        core = random_problem(300 + seed)
        config = cl.SolverConfig(stop={"time_limit_seconds": 30, "step_limit": 300}, run={"random_seed": seed})
        solved = cl.solve(ContainerLoadingProblemModel.from_core(core), config=config)
        again = solved.to_core()
        back = cl.calculate_score(again)
        if not solved.unplaced_items:
            assert (back.hard, back.soft) == (solved.score_hard, solved.score_soft)
        assert len(again.unplaced_items()) <= len(solved.unplaced_items)


def test_initial_items_are_the_starting_point_and_stay_on_the_result():
    model = small()
    model.containers[1].initial_items = ["i8", "i7"]
    config = cl.SolverConfig(stop={"time_limit_seconds": 0})
    solved = cl.solve(model, config=config)
    assert solved.containers[1].items[:2] == ["i8", "i7"]
    assert solved.containers[1].initial_items == ["i8", "i7"]
    assert solved.containers[0].initial_items == []


def test_from_core_describes_an_unsolved_problem_too():
    core = cl.generate_demo_data(item_count=6, container_count=2)
    model = ContainerLoadingProblemModel.from_core(core)
    assert model.containers_used == 0 and len(model.unplaced_items) == 6
    assert all(container.items == [] for container in model.containers)
    assert (model.score_hard, model.score_soft) == (0, 0.0)
    assert model.items[0].rotations in ("all", "upright") and model.containers[0].max_weight == 600000
    assert model.to_core().loading() == [[], []]


def test_solve_request_is_solve_with_the_request_settings():
    request = ContainerLoadingRequest(
        model_input=small(), time_limit_seconds=30, random_seed=4, solver_parameters={"stop": {"step_limit": 200}}
    )
    solved = solve_request(request)
    assert (solved.score_hard, solved.score_soft) == (0, -2.0)
    data = {"model_input": small().model_dump(), "timelimit": 0.2, "num_workers": 2}
    assert solve_request(ContainerLoadingRequest.model_validate(data)).score_hard == 0


@pytest.mark.parametrize(
    ("change", "message"),
    [
        (lambda data: data["containers"].clear(), "at least 1 item"),
        (lambda data: data["containers"].append(dict(data["containers"][0])), "container ids must be unique"),
        (lambda data: data["items"].append(dict(data["items"][0])), "item ids must be unique"),
        (lambda data: data["items"][0].update(length=0), "greater than 0"),
        (lambda data: data["items"][0].update(height=1_000_001), "less than or equal to 1000000"),
        (lambda data: data["items"][0].update(weight=-1), "greater than or equal to 0"),
        (lambda data: data["items"][0].update(rotations="any"), "'all', 'upright' or 'fixed'"),
        (lambda data: data["items"][0].update(max_load_on_top=-5), "greater than or equal to 0"),
        (lambda data: data["items"][0].update(colour="red"), "Extra inputs are not permitted"),
        (lambda data: data["containers"][0].update(cost=-1), "greater than or equal to 0"),
        (lambda data: data["containers"][0].update(cost=float("inf")), "finite number"),
        (lambda data: data["containers"][0].update(max_weight=-1), "greater than or equal to 0"),
        (lambda data: data["containers"][0].update(max_weight=2**63), "less than or equal to 9223372036854775807"),
        (lambda data: data["items"][0].update(max_load_on_top=2**63), "less than or equal to 9223372036854775807"),
        (lambda data: data["containers"][0].update(initial_items=["nobody"]), "unknown item 'nobody'"),
        (
            lambda data: data["containers"][0].update(initial_items=["stop1-crate", "stop1-crate"]),
            "appears twice in the loading orders",
        ),
        (lambda data: data["containers"][0].update(items=["nobody"]), "unknown item 'nobody'"),
        (lambda data: data.update(min_support_ratio=0), "greater than 0"),
        (lambda data: data.update(min_support_ratio=1.2), "less than or equal to 1"),
        (lambda data: data.update(weights={"used_container_cost": 2}), "Extra inputs are not permitted"),
        (lambda data: data["items"][0].update(max_stack_layers=0), "greater than or equal to 1"),
        (lambda data: data["items"][0].update(stack_group=""), "at least 1 character"),
        (lambda data: data["containers"][0].update(max_stack_height=0), "greater than 0"),
        (lambda data: data["containers"][0].update(max_offset_length=1.5), "less than or equal to 1"),
        (lambda data: data["containers"][0].update(max_offset_width=-0.1), "greater than or equal to 0"),
        (lambda data: data.update(stacking_rules=[{"top": "", "bottom": "food"}]), "at least 1 character"),
        (lambda data: data.update(stacking_rules=[{"top": "a", "bottom": "b", "allowed": True}]), "Extra inputs"),
    ],
)
def test_a_bad_model_is_refused(change, message):
    data = van()
    change(data)
    with pytest.raises(ValidationError, match=message):
        ContainerLoadingProblemModel.model_validate(data)


def test_an_empty_group_is_refused() -> None:
    with pytest.raises(ValidationError):
        ItemModel(id="a", length=1, width=1, height=1, group="")


def test_the_stacking_inputs_go_through_to_the_engine_and_back():
    """Every new input reaches the engine (it decodes by them) and comes back on the result."""
    model = ContainerLoadingProblemModel.model_validate(
        {
            "containers": [
                {"id": "c", "length": 8, "width": 4, "height": 10, "max_stack_height": 8, "max_offset_width": 0.5}
            ],
            "items": [
                {"id": "food", "length": 4, "width": 4, "height": 4, "rotations": "fixed", "stack_group": "food"},
                {"id": "chem", "length": 4, "width": 4, "height": 4, "rotations": "fixed", "stack_group": "chem"},
                {"id": "pallet", "length": 4, "width": 4, "height": 4, "rotations": "fixed", "bottom_only": True},
                {"id": "box", "length": 4, "width": 4, "height": 4, "rotations": "fixed", "max_stack_layers": 2},
            ],
            "stacking_rules": [{"top": "chem", "bottom": "food"}],
        }
    )
    core = model.to_core()
    assert core.stacking_rules == [("chem", "food")]
    assert (core.containers[0].max_stack_height, core.containers[0].max_offset_width) == (8, 0.5)
    assert (core.items[2].bottom_only, core.items[3].max_stack_layers, core.items[0].stack_group) == (True, 2, "food")
    solved = cl.solve(model, time_limit_seconds=1.0, random_seed=0)
    assert solved.score_hard == 0 and solved.unplaced_items == []
    by_id = {item.id: item for item in solved.items}
    assert by_id["pallet"].z == 0
    assert all(item.z + item.placed_height <= 8 for item in solved.items)
    assert not (by_id["chem"].z == 4 and (by_id["chem"].x, by_id["chem"].y) == (by_id["food"].x, by_id["food"].y))
    assert solved.stacking_rules == model.stacking_rules
    assert ContainerLoadingProblemModel.from_core(solved.to_core()).containers[0].max_stack_height == 8

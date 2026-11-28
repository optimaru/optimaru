"""optimaru.box_sizing.models — the JSON contract."""

import json

import pytest
from pydantic import ValidationError

import optimaru.box_sizing as bs
from optimaru.box_sizing.models import BoxSizingProblemModel, BoxSizingRequest

from .conftest import nested_data


def boxes_of(solved) -> dict:
    return {parcel.id: parcel.box for parcel in solved.parcels}


def test_the_field_names_are_the_contract():
    """The WMS backend sends and reads these names: a rename is a breaking change."""
    assert list(BoxSizingProblemModel.model_fields) == [
        "boxes",
        "parcels",
        "max_box_types",
        "rate_tables",
        "warm_start",
        "score_hard",
        "score_soft",
        "box_types_used",
        "void_volume",
        "fill_ratio",
        "board_area",
        "shipping_cost",
    ]
    assert list(bs.models.BoxModel.model_fields) == [
        "id",
        "length",
        "width",
        "height",
        "max_weight",
        "cost",
        "required",
        "dim_divisor",
        "parcels",
        "billed_weight",
        "shipping_cost",
    ]
    assert list(bs.models.ParcelModel.model_fields) == [
        "id",
        "length",
        "width",
        "height",
        "weight",
        "quantity",
        "zone",
        "box",
    ]
    assert list(bs.models.RateTableModel.model_fields) == ["zone", "breaks"]
    assert list(bs.models.RateBreakModel.model_fields) == ["max_weight", "price"]
    assert "model_input" in BoxSizingRequest.model_fields


def test_the_defaults(nested):
    model = nested()
    box, parcel = model.boxes[0], model.parcels[2]
    assert (box.max_weight, box.cost, box.required, box.dim_divisor, box.parcels) == (None, 0.0, False, None, None)
    assert (box.billed_weight, box.shipping_cost) == (None, None)
    assert (parcel.weight, parcel.quantity, parcel.zone, parcel.box) == (0, 1, None, None)
    assert (model.max_box_types, model.rate_tables, model.warm_start) == (None, [], False)
    assert (model.score_hard, model.box_types_used, model.void_volume, model.fill_ratio) == (None, None, None, None)
    assert (model.board_area, model.shipping_cost) == (None, None)


def test_solve_takes_the_model_and_returns_a_solved_model(nested):
    problem = nested(max_box_types=2)
    before = problem.model_dump()
    solved = bs.solve(problem, time_limit_seconds=0.2, random_seed=1)
    assert isinstance(solved, BoxSizingProblemModel) and solved is not problem
    assert problem.model_dump() == before
    # L is needed; M serves the small and the medium parcels: 10 x 7,000 + 5 x 2,000.
    assert boxes_of(solved) == {"small": "M", "medium": "M", "large": "L"}
    assert (solved.score_hard, solved.score_soft) == (0, -80000.0)
    assert (solved.box_types_used, solved.void_volume) == (2, 80000)
    assert solved.fill_ratio == pytest.approx((10 * 1000 + 5 * 6000 + 27000) / (15 * 8000 + 27000))
    assert [box.parcels for box in solved.boxes] == [0, 15, 1]
    # 2 (lw + lh + wh): M 2,400, L 5,400. Without a divisor or rate tables nothing is billed.
    assert solved.board_area == 15 * 2400 + 5400
    assert solved.shipping_cost is None and {(box.billed_weight, box.shipping_cost) for box in solved.boxes} == {
        (None, None)
    }


def priced_data(**fields) -> dict:
    """The nested problem, billed: divisor 10 (S bills 100, M 800, L 2,700) and two zones."""
    data = nested_data(**fields)
    for box in data["boxes"]:
        box["dim_divisor"] = 10
    data["parcels"][0] |= {"weight": 50}  # bills 100 in S, 800 in M
    data["parcels"][1] |= {"weight": 900, "zone": "far"}  # bills 900 in M, 2,700 in L
    data["parcels"][2] |= {"weight": 3000}  # bills 3,000 in L
    data["rate_tables"] = [
        {
            "breaks": [
                {"max_weight": 500, "price": 2.0},
                {"max_weight": 1000, "price": 3.0},
                {"max_weight": 5000, "price": 8.0},
            ]
        },
        {"zone": "far", "breaks": [{"max_weight": 1000, "price": 5.0}, {"max_weight": 5000, "price": 12.0}]},
    ]
    return data


def test_billed_weight_and_shipping_cost_are_reported_per_box():
    model = BoxSizingProblemModel.model_validate(priced_data(max_box_types=2))
    solved = bs.solve(model, time_limit_seconds=0.1, random_seed=1)
    assert boxes_of(solved) == {"small": "M", "medium": "M", "large": "L"}
    assert [box.billed_weight for box in solved.boxes] == [0, 10 * 800 + 5 * 900, 3000]
    assert [box.shipping_cost for box in solved.boxes] == [0.0, 10 * 3.0 + 5 * 5.0, 8.0]
    assert solved.shipping_cost == 63.0
    # At the default weights the shipping cost is reported, not scored.
    assert solved.score_soft == -80000.0


def test_the_shipping_cost_moves_parcels_when_it_is_picked():
    """S (bills 100) ships the small parcel at 2 against 3 in M: worth 30 of price, which a
    weight of 10,000 makes worth more than the 70,000 of air the third size saves."""
    from optimaru.box_sizing.constraints import BoxTypeLimitCost, ShippingCost, get_constraints

    model = BoxSizingProblemModel.model_validate(priced_data(max_box_types=3))
    picks = [ShippingCost(weight=10_000) if spec.name == "shipping_cost" else spec for spec in get_constraints()]
    solved = bs.solve(model, time_limit_seconds=0.1, random_seed=1, constraints=picks)
    assert boxes_of(solved)["small"] == "S" and solved.shipping_cost == 10 * 2.0 + 5 * 5.0 + 8.0
    assert BoxTypeLimitCost  # the limit of 3 holds every size


def test_only_dim_divisors_report_the_billed_weight_but_no_price(nested):
    data = nested_data()
    data["boxes"][0]["dim_divisor"] = 1  # S bills 1,000 per shipment
    solved = bs.solve(BoxSizingProblemModel.model_validate(data), time_limit_seconds=0)
    assert [box.billed_weight for box in solved.boxes] == [10 * 1000, 0, 0]
    assert solved.shipping_cost is None and solved.boxes[0].shipping_cost is None


@pytest.mark.parametrize(
    ("edit", "message"),
    [
        (lambda d: d["rate_tables"].append(dict(d["rate_tables"][1])), "zone 'far' is given twice"),
        (
            lambda d: d["rate_tables"][0]["breaks"].reverse(),
            "the rate table without a zone: the breaks' max_weight must be strictly ascending",
        ),
        (lambda d: d["parcels"][1].update(zone="near"), "parcel 'medium': no rate table has zone 'near'"),
        (lambda d: d["rate_tables"].pop(0), "parcel 'small': no zone, and no rate table without a zone"),
        (lambda d: d.update(rate_tables=[]), "parcel 'medium': no rate table has zone 'far'"),
    ],
)
def test_rate_tables_that_cannot_price_every_parcel_are_refused(edit, message):
    data = priced_data()
    edit(data)
    with pytest.raises(ValidationError, match=message):
        BoxSizingProblemModel.model_validate(data)


def test_a_billed_weight_above_the_top_break_is_refused_by_the_engine():
    data = priced_data()
    data["rate_tables"][0]["breaks"].pop()  # the zone-less table now stops at 1,000; L bills 2,700
    model = BoxSizingProblemModel.model_validate(data)
    with pytest.raises(ValueError, match="parcel 'small' bills 2700 in box 'L', above the top break \\(1000\\)"):
        bs.solve(model, time_limit_seconds=0)


def test_json_round_trip_preserves_everything(nested):
    solved = bs.solve(nested(max_box_types=2), time_limit_seconds=0.1)
    clone = BoxSizingProblemModel.model_validate_json(solved.model_dump_json())
    assert clone == solved
    assert json.loads(solved.model_dump_json())["parcels"][0]["box"] == "M"


def test_the_core_round_trip_keeps_the_facts(nested):
    data = nested_data(max_box_types=3, warm_start=True)
    data["boxes"][1] |= {"max_weight": 500, "cost": 2.5, "required": True}
    data["parcels"][0] |= {"weight": 120, "box": "L"}
    model = BoxSizingProblemModel.model_validate(data)
    core = model.to_core()
    assert (core.boxes[1].max_weight, core.boxes[1].cost, core.boxes[1].required) == (500, 2.5, True)
    assert (core.parcels[0].weight, core.parcels[0].quantity, core.parcels[0].box) == (120, 10, 2)
    assert (core.max_box_types, core.warm_start) == (3, True)
    back = BoxSizingProblemModel.from_core(core)
    assert back.model_dump(include={"boxes": {"__all__": {"id", "max_weight", "cost", "required"}}}) == (
        model.model_dump(include={"boxes": {"__all__": {"id", "max_weight", "cost", "required"}}})
    )
    assert [parcel.box for parcel in back.parcels] == ["L", None, None]


def test_the_core_round_trip_keeps_the_rates():
    model = BoxSizingProblemModel.model_validate(priced_data())
    core = model.to_core()
    assert core.boxes[0].dim_divisor == 10 and core.parcels[1].zone == "far"
    assert [(table.zone, [(b.max_weight, b.price) for b in table.breaks]) for table in core.rate_tables] == [
        (None, [(500, 2.0), (1000, 3.0), (5000, 8.0)]),
        ("far", [(1000, 5.0), (5000, 12.0)]),
    ]
    back = BoxSizingProblemModel.from_core(core)
    assert back.rate_tables == model.rate_tables
    assert [parcel.zone for parcel in back.parcels] == [None, "far", None]
    assert [box.dim_divisor for box in back.boxes] == [10, 10, 10]


def test_extra_fields_are_rejected():
    with pytest.raises(ValidationError):
        BoxSizingProblemModel.model_validate(nested_data(void_weight=2))
    data = nested_data()
    data["boxes"][0]["depth"] = 3
    with pytest.raises(ValidationError):
        BoxSizingProblemModel.model_validate(data)


@pytest.mark.parametrize(
    ("where", "field", "value"),
    [
        ("boxes", "length", 0),
        ("boxes", "height", 2_000_001),
        ("boxes", "max_weight", -1),
        ("boxes", "cost", -0.5),
        ("boxes", "cost", float("inf")),
        ("boxes", "id", ""),
        ("parcels", "width", 0),
        ("parcels", "weight", -1),
        ("parcels", "quantity", 0),
        ("parcels", "quantity", 2**53 + 1),
        ("boxes", "dim_divisor", 0),
        ("parcels", "zone", ""),
    ],
)
def test_a_fact_out_of_range_is_refused(where, field, value):
    data = nested_data()
    data[where][0][field] = value
    with pytest.raises(ValidationError):
        BoxSizingProblemModel.model_validate(data)


def test_at_least_one_candidate_box():
    with pytest.raises(ValidationError):
        BoxSizingProblemModel.model_validate({"boxes": [], "parcels": []})
    assert BoxSizingProblemModel.model_validate(nested_data(parcels=[])).parcels == []


@pytest.mark.parametrize("value", [0, -1])
def test_max_box_types_is_at_least_one(value):
    with pytest.raises(ValidationError):
        BoxSizingProblemModel.model_validate(nested_data(max_box_types=value))


def test_ids_are_unique():
    data = nested_data()
    data["boxes"][1]["id"] = "S"
    with pytest.raises(ValidationError, match="box ids must be unique"):
        BoxSizingProblemModel.model_validate(data)
    data = nested_data()
    data["parcels"][1]["id"] = "small"
    with pytest.raises(ValidationError, match="parcel ids must be unique"):
        BoxSizingProblemModel.model_validate(data)


def test_a_parcel_that_fits_no_candidate_is_refused_naming_it():
    data = nested_data()
    data["parcels"].append({"id": "pole", "length": 31, "width": 5, "height": 5})
    with pytest.raises(ValueError, match=r"parcel 'pole' \(31 x 5 x 5, weight 0\) fits none of the candidate boxes"):
        BoxSizingProblemModel.model_validate(data)


def test_the_refusal_counts_the_parcels_that_fit_nothing_and_names_five_more():
    data = nested_data()
    data["parcels"] += [{"id": f"pole-{i}", "length": 31 + i, "width": 5, "height": 5} for i in range(8)]
    with pytest.raises(ValueError) as refusal:
        BoxSizingProblemModel.model_validate(data)
    assert (
        "parcel 'pole-0' (31 x 5 x 5, weight 0) fits none of the candidate boxes: add a box that takes it, or "
        "leave it out (and 7 more: 'pole-1', 'pole-2', 'pole-3', 'pole-4', 'pole-5', ...)"
    ) in str(refusal.value)
    # The engine says the same of a compiled problem.
    core = bs.BoxSizingProblem(
        [bs.Box("S", 10, 10, 10)], [bs.Parcel(f"pole-{i}", 11 + i, 5, 5) for i in range(3)] + [bs.Parcel("ok", 5, 5, 5)]
    )
    with pytest.raises(ValueError) as refusal:
        bs.solve(core, time_limit_seconds=0)
    assert str(refusal.value) == (
        "parcel 'pole-0' (11 x 5 x 5, weight 0) fits none of the candidate boxes: add a box that takes it, or "
        "leave it out (and 2 more: 'pole-1', 'pole-2')"
    )


def test_a_parcel_too_heavy_for_every_box_that_holds_it_is_refused():
    data = nested_data()
    data["boxes"][2]["max_weight"] = 1000
    data["parcels"][2]["weight"] = 1001
    with pytest.raises(ValueError, match="parcel 'large' .* fits none of the candidate boxes"):
        BoxSizingProblemModel.model_validate(data)
    data["parcels"][2]["weight"] = 1000
    assert BoxSizingProblemModel.model_validate(data)


def test_the_fit_scan_tries_the_largest_box_first_and_still_finds_every_misfit():
    """A parcel that fits only a small box with a higher weight limit is not refused because
    the larger boxes come first in the scan."""
    data = nested_data()
    data["boxes"][1]["max_weight"] = data["boxes"][2]["max_weight"] = 100  # M and L carry 100
    data["parcels"][0]["weight"] = 500  # the small parcel: only S (no limit) takes it
    solved = bs.solve(BoxSizingProblemModel.model_validate(data), time_limit_seconds=0)
    assert solved.parcels[0].box == "S"
    data["boxes"][0]["max_weight"] = 499
    with pytest.raises(ValueError, match="parcel 'small' .* fits none of the candidate boxes"):
        BoxSizingProblemModel.model_validate(data)


def test_a_solved_model_is_not_scanned_again_but_a_model_from_json_is():
    """from_core reads a problem the engine has checked; the same data read as JSON is
    validated in full."""
    core = bs.BoxSizingProblem([bs.Box("S", 10, 10, 10)], [bs.Parcel("pole", 11, 5, 5, box=0)])
    bs.calculate_score(core)
    model = BoxSizingProblemModel.from_core(core)  # scored, not solved: the misfit is on the score
    assert model.score_hard == -1
    with pytest.raises(ValueError, match="fits none of the candidate boxes"):
        BoxSizingProblemModel.model_validate_json(model.model_dump_json())


def test_a_parcel_may_be_turned_to_fit():
    data = nested_data()
    data["boxes"] = [{"id": "flat", "length": 300, "width": 200, "height": 100}]
    data["parcels"] = [{"id": "upright", "length": 100, "width": 300, "height": 200}]
    solved = bs.solve(BoxSizingProblemModel.model_validate(data), time_limit_seconds=0)
    assert (solved.score_hard, solved.void_volume, solved.fill_ratio) == (0, 0, 1.0)


def test_the_engine_refuses_the_same_parcel_on_a_compiled_problem():
    problem = bs.BoxSizingProblem([bs.Box("S", 10, 10, 10)], [bs.Parcel("pole", 11, 5, 5)])
    with pytest.raises(ValueError, match=r"parcel 'pole' \(11 x 5 x 5, weight 0\) fits none of the candidate boxes"):
        bs.solve(problem, time_limit_seconds=0)
    assert (
        bs.calculate_score(bs.BoxSizingProblem([bs.Box("S", 10, 10, 10)], [bs.Parcel("pole", 11, 5, 5, box=0)])).hard
        == -1
    )


def test_more_required_boxes_than_the_limit_is_refused():
    data = nested_data(max_box_types=1)
    data["boxes"][0]["required"] = data["boxes"][2]["required"] = True
    with pytest.raises(ValueError, match="max_box_types is 1, but 2 boxes are required"):
        BoxSizingProblemModel.model_validate(data)


def test_a_box_that_is_not_a_candidate_is_refused():
    data = nested_data()
    data["parcels"][0]["box"] = "XL"
    with pytest.raises(ValueError, match="parcel 'small': box 'XL' is not one of the candidate boxes"):
        BoxSizingProblemModel.model_validate(data)


def test_a_fact_edited_after_validation_is_refused_by_to_core(nested):
    model = nested()
    model.parcels[0].box = "XL"
    with pytest.raises(ValueError, match="is not one of the candidate boxes"):
        model.to_core()
    with pytest.raises(ValueError, match="is not one of the candidate boxes"):
        bs.solve(model, time_limit_seconds=0)


def test_a_volume_a_double_cannot_hold_is_refused_by_the_engine():
    data = {
        "boxes": [{"id": "hall", "length": 2_000_000, "width": 2_000_000, "height": 2_000_000}],
        "parcels": [{"id": "p", "length": 1, "width": 1, "height": 1, "quantity": 2}],
    }
    with pytest.raises(ValueError, match="use a coarser length unit"):
        bs.solve(BoxSizingProblemModel.model_validate(data), time_limit_seconds=0)


def test_solve_refuses_anything_but_the_problem_or_its_model(nested):
    with pytest.raises(TypeError, match="BoxSizingProblem or a models.BoxSizingProblemModel"):
        bs.solve(nested().model_dump())


def test_the_demo_instance_is_reproducible_and_valid():
    first, again = bs.generate_demo_data(), bs.generate_demo_data()
    assert first == again and first != bs.generate_demo_data(seed=1)
    assert (len(first.boxes), len(first.parcels), first.max_box_types) == (24, 120, 6)
    assert len(bs.generate_demo_data(parcel_count=10, max_box_types=None).parcels) == 10
    assert bs.generate_demo_data is bs.demo.generate_demo_data

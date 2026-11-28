"""The small problems the box-sizing tests share."""

import pytest

from optimaru.box_sizing.models import BoxSizingProblemModel


def nested_data(**fields) -> dict:
    """Three sizes that nest (S in M in L, volumes 1,000 / 8,000 / 27,000) and a parcel for each."""
    return {
        "boxes": [
            {"id": "S", "length": 10, "width": 10, "height": 10},
            {"id": "M", "length": 20, "width": 20, "height": 20},
            {"id": "L", "length": 30, "width": 30, "height": 30},
        ],
        "parcels": [
            {"id": "small", "length": 10, "width": 10, "height": 10, "quantity": 10},
            {"id": "medium", "length": 20, "width": 20, "height": 15, "quantity": 5},
            {"id": "large", "length": 30, "width": 30, "height": 30},
        ],
    } | fields


@pytest.fixture
def nested():
    """A builder: ``nested(max_box_types=2)`` is the nested problem with that field set."""
    return lambda **fields: BoxSizingProblemModel.model_validate(nested_data(**fields))

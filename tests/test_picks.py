"""The pick-list helpers of the tests refuse a name that picks nothing or more than one."""

from dataclasses import dataclass

import pytest

from ._picks import only, replace_default, without


@dataclass(frozen=True)
class _Spec:
    name: str
    weight: float | None = None


SPECS = [_Spec("a_cost"), _Spec("b_cost"), _Spec("c_cost")]


def test_the_helpers_keep_the_order_and_change_one_pick():
    assert replace_default(SPECS, "b_cost", _Spec("b_cost", 2)) == [
        _Spec("a_cost"),
        _Spec("b_cost", 2),
        _Spec("c_cost"),
    ]
    assert without(SPECS, "a_cost") == SPECS[1:]
    assert only(SPECS, "c_cost", "a_cost") == [_Spec("a_cost"), _Spec("c_cost")]


@pytest.mark.parametrize("helper", [replace_default, without, only])
def test_a_misspelt_name_fails(helper):
    args = (SPECS, "b_cots", _Spec("b_cost", 2)) if helper is replace_default else (SPECS, "b_cots")
    with pytest.raises(AssertionError, match="'b_cots' names 0 of the picks"):
        helper(*args)


def test_a_name_picked_twice_fails():
    with pytest.raises(AssertionError, match="names 2 of the picks"):
        without([*SPECS, _Spec("a_cost", 3)], "a_cost")

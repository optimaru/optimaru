"""The demo functions' names before 0.5.1 still import and call, as `generate_demo_data` itself.

tests/test_layout.py checks from the source that every demo function is called
`generate_demo_data`; this checks the compiled modules: each former name is importable from every
module it was importable from before, is the same object as `generate_demo_data` there, and is
left out of `__all__`.
"""

import importlib
import warnings

import pytest

from ._build import use_case_module

# (use case, module under optimaru.<use case>, the name before 0.5.1)
FORMER_NAMES = [
    ("conference_scheduling", "", "demo_conference"),
    ("conference_scheduling", ".demo", "demo_conference"),
    ("meeting_scheduling", "", "demo_data"),
    ("meeting_scheduling", ".models", "demo_data"),
    ("meeting_scheduling", "._core", "demo_data"),
    ("order_picking", "", "build_demo_data"),
    ("order_picking", "._core", "build_demo_data"),
    ("sports_league_scheduling", "", "demo_schedule"),
    ("sports_league_scheduling", ".demo", "demo_schedule"),
    ("tournament_scheduling", ".models", "demo_data"),
]


@pytest.mark.parametrize(("use_case", "submodule", "former"), FORMER_NAMES)
def test_a_former_demo_name_is_generate_demo_data(use_case, submodule, former):
    use_case_module(use_case)
    module = importlib.import_module(f"optimaru.{use_case}{submodule}")
    namespace = {}
    exec(f"from optimaru.{use_case}{submodule} import {former}", namespace)
    assert namespace[former] is module.generate_demo_data
    assert getattr(module, former) is module.generate_demo_data
    if hasattr(module, "__all__"):
        assert "generate_demo_data" in module.__all__
        assert former not in module.__all__
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        assert getattr(module, former)() is not None

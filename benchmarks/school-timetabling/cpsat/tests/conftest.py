"""Shared fixtures borrowed from the use case's own test suite.

`test_engine_parity.py` solves the use case's canonical demo school with both
backends, so it reuses `use-cases/school-timetabling/python/tests/conftest.py`
instead of keeping a second copy of the demo problem.
"""

import importlib.util
from pathlib import Path

import pytest

_USE_CASE_CONFTEST = Path(__file__).resolve().parents[4] / "use-cases" / "school-timetabling" / "python" / "tests" / "conftest.py"
_spec = importlib.util.spec_from_file_location("st_use_case_conftest", _USE_CASE_CONFTEST)
_module = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_module)


@pytest.fixture
def problem() -> dict:
    return _module.demo_problem()

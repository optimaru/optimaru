"""What every test starts from."""

import pytest

from optimaru import _api


@pytest.fixture(autouse=True)
def solve_on_this_machine(monkeypatch):
    """No test solves on the optimaru API by accident: a key in the environment is set aside.

    tests/test_api.py switches the API on itself, against a stand-in on a local port.
    """
    monkeypatch.delenv(_api.API_KEY_VARIABLE, raising=False)
    monkeypatch.delenv(_api.API_URL_VARIABLE, raising=False)
    for setting in ("_chosen", "_key", "_url"):
        monkeypatch.setattr(_api, setting, None)
    monkeypatch.setattr(_api, "_said", set())

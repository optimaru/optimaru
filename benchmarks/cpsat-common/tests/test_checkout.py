import importlib
import sys
import types

import pytest
from cpsat_plugins import checkout


@pytest.fixture
def fake_case(tmp_path, monkeypatch):
    """A use-case-shaped package that imports its submodules relatively."""
    package = tmp_path / "fake_case"
    package.mkdir()
    (package / "__init__.py").write_text("from .models import Problem\n")
    (package / "models.py").write_text("class Problem:\n    pass\n")
    (package / "constraints.py").write_text("from .models import Problem\n")
    monkeypatch.syspath_prepend(str(tmp_path))
    monkeypatch.setitem(checkout.SUBPACKAGES, "fake_case", "fake_subpackage")
    names = ("optimaru", "fake_case", "fake_case.models", "fake_case.constraints")
    saved = {name: sys.modules.pop(name, None) for name in names}
    yield
    for name in list(sys.modules):
        if name.startswith(("optimaru.fake_subpackage", "fake_case")):
            del sys.modules[name]
    for name, module in saved.items():
        if module is not None:
            sys.modules[name] = module
        else:
            sys.modules.pop(name, None)


def test_the_checkout_package_answers_to_the_published_names(fake_case):
    checkout.use_checkout_engine("fake_case")
    import fake_case

    published = importlib.import_module("optimaru.fake_subpackage")
    assert published is fake_case
    # Submodules not imported by the package itself are registered too, so the
    # library never loads a second copy of a class.
    assert (
        importlib.import_module("optimaru.fake_subpackage.constraints")
        is sys.modules["fake_case.constraints"]
    )
    from optimaru.fake_subpackage.models import Problem

    assert Problem is fake_case.Problem
    checkout.use_checkout_engine("fake_case")  # idempotent


def test_an_engine_imported_from_elsewhere_is_refused(fake_case):
    sys.modules["optimaru.fake_subpackage"] = types.ModuleType(
        "optimaru.fake_subpackage"
    )
    with pytest.raises(RuntimeError, match="before importing external_solver"):
        checkout.use_checkout_engine("fake_case")

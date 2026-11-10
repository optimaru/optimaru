import sys

import pytest

from cpsat_plugins import checkout


@pytest.fixture
def fake_engine(tmp_path, monkeypatch):
    """An ``optimaru`` package with one use case, ``fake_case``, under tmp_path/repo."""
    repo = tmp_path / "repo"
    package = repo / "optimaru" / "fake_case"
    package.mkdir(parents=True)
    (repo / "optimaru" / "__init__.py").write_text("")
    (package / "__init__.py").write_text("")
    monkeypatch.syspath_prepend(str(repo))
    saved = {name: sys.modules.pop(name) for name in list(sys.modules) if name.split(".")[0] == "optimaru"}
    yield repo
    for name in [name for name in sys.modules if name.split(".")[0] == "optimaru"]:
        del sys.modules[name]
    sys.modules.update(saved)


def test_the_checkout_engine_is_accepted(fake_engine):
    module = checkout.use_checkout_engine("fake_case", repo_root=fake_engine)

    assert module is sys.modules["optimaru.fake_case"]
    assert checkout.use_checkout_engine("fake_case", repo_root=fake_engine) is module  # idempotent


def test_an_engine_from_elsewhere_is_refused(fake_engine, tmp_path):
    elsewhere = tmp_path / "another-checkout"
    elsewhere.mkdir()

    with pytest.raises(RuntimeError, match="not from the checkout"):
        checkout.use_checkout_engine("fake_case", repo_root=elsewhere)


def test_the_repo_root_is_this_checkout():
    assert (checkout.REPO_ROOT / "optimaru" / "__init__.py").is_file()
    assert (checkout.REPO_ROOT / "benchmarks" / "cpsat-common").is_dir()

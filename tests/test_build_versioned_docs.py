"""scripts/build_versioned_docs.py: which tags become versions, and the redirects of the old addresses."""

from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest

_SCRIPT = Path(__file__).resolve().parent.parent / "scripts" / "build_versioned_docs.py"
_spec = importlib.util.spec_from_file_location("build_versioned_docs", _SCRIPT)
docs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(docs)


def test_newest_patch_per_minor_newest_first():
    tags = ["v0.3.9", "v0.4.0", "v0.4.2", "v0.9.0", "v0.10.0", "v1.0.0rc1", "not-a-tag"]
    assert docs.pick_releases(tags) == [("0.10", "v0.10.0"), ("0.9", "v0.9.0"), ("0.4", "v0.4.2")]


def _page(root: Path, rel: str) -> None:
    (root / rel).mkdir(parents=True, exist_ok=True)
    (root / rel / "index.html").write_text("<html></html>")


def test_redirects_lead_to_latest(tmp_path):
    for rel in ("", "reference/knapsack"):
        _page(tmp_path / "latest", rel)
    count = docs.write_redirects(tmp_path, "https://example.org/docs/", "0.9", {"dev", "0.9"})
    assert count == 2
    page = (tmp_path / "reference" / "knapsack" / "index.html").read_text()
    assert 'url=../../latest/reference/knapsack/"' in page
    assert 'href="https://example.org/docs/0.9/reference/knapsack/"' in page
    assert "url=latest/" in (tmp_path / "index.html").read_text()


@pytest.mark.parametrize("name", ["dev", "0.8", "latest"])
def test_a_page_may_not_shadow_a_version_folder(tmp_path, name):
    _page(tmp_path / "latest", name)
    with pytest.raises(SystemExit):
        docs.write_redirects(tmp_path, "https://example.org/docs/", "0.9", {"dev", "0.9", "0.8"})

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


def test_a_release_tree_links_its_tag_and_gets_the_banner(tmp_path):
    hooks = tmp_path / "scripts" / "mkdocs_hooks.py"
    hooks.parent.mkdir()
    hooks.write_text('url = f"{GITHUB}/{kind}/main/{repo_path}"\n')
    (tmp_path / "mkdocs.yml").write_text("site_name: optimaru\n")
    docs.prepare_release_tree(tmp_path, "v0.4.3")
    assert "/{kind}/v0.4.3/" in hooks.read_text()
    assert (tmp_path / "overrides" / "main.html").is_file()
    # A tag without custom_dir gets one; a release has no edit link; dev keeps its own config.
    assert docs.release_overrides(tmp_path, "v0.4.3") == {"edit_uri": "", "theme": {"custom_dir": "overrides"}}
    (tmp_path / "mkdocs.yml").write_text("theme:\n  custom_dir: overrides\n")
    assert docs.release_overrides(tmp_path, "v0.9.0") == {"edit_uri": ""}
    assert docs.release_overrides(tmp_path, "main") == {}


def test_a_tag_keeps_its_own_banner_template(tmp_path):
    (tmp_path / "overrides").mkdir()
    (tmp_path / "overrides" / "main.html").write_text("own")
    docs.prepare_release_tree(tmp_path, "v0.9.0")
    assert (tmp_path / "overrides" / "main.html").read_text() == "own"

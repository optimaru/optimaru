"""Every list of use cases agrees with the subpackages.

A use case is a subpackage optimaru/<use case>/ with a _core.pyi stub (setup.py
builds exactly those). The lists below name the use cases by hand; when one of
them misses a new use case, its C++ or Python tests silently stop running in CI.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

import optimaru

ROOT = Path(__file__).resolve().parent.parent
USE_CASES = {path.parent.name for path in (ROOT / "optimaru").glob("*/_core.pyi")}


def _matrix(workflow: str, key: str) -> set[str]:
    """The values of the CI matrix list `key:` in a workflow file (plain YAML list items)."""
    lists = re.findall(rf"^(\s+){key}:\n((?:\1  - .+\n)+)", (ROOT / workflow).read_text(), re.M)
    assert lists, f"{workflow}: no matrix list {key!r}"
    return {item for _, block in lists for item in re.findall(r"- (\S+)", block)}


def test_every_subpackage_is_a_use_case():
    assert len(USE_CASES) == 22
    assert all((ROOT / "optimaru" / name / "cpp" / "CMakeLists.txt").is_file() for name in USE_CASES)
    assert all((ROOT / "tests" / name).is_dir() for name in USE_CASES)


def test_the_package_lists_every_use_case():
    assert set(optimaru.__all__) == USE_CASES


def test_cmake_builds_every_use_case():
    text = (ROOT / "CMakeLists.txt").read_text()
    block = re.search(r"set\(OPTIMARU_ALL_USE_CASES\s+([^)]*)\)", text)
    assert block, "CMakeLists.txt: no OPTIMARU_ALL_USE_CASES"
    assert set(block.group(1).split()) == USE_CASES


def test_ci_tests_every_use_case():
    assert _matrix(".github/workflows/ci.yml", "use_case") == USE_CASES


def test_ci_benchmarks_every_use_case():
    # vehicle-routing has its own job (the OR-Tools Routing comparison); cpsat-common is the shared runner.
    benchmarks = _matrix(".github/workflows/ci.yml", "benchmark") - {"cpsat-common"}
    assert benchmarks | {"vehicle-routing"} == {name.replace("_", "-") for name in USE_CASES}


def test_every_use_case_has_a_guide_and_a_benchmark():
    for name in USE_CASES:
        slug = name.replace("_", "-")
        assert (ROOT / "docs" / "use-cases" / slug / "README.md").is_file(), slug
        assert (ROOT / "benchmarks" / slug).is_dir(), slug


def test_ruff_is_configured_in_one_place():
    """The root pyproject.toml configures ruff for every directory, the benchmark projects
    included, so an import sorts the same way wherever ruff runs from."""
    tracked = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True)
    if tracked.returncode != 0:
        pytest.skip("not a git checkout")
    strays = []
    for name in tracked.stdout.splitlines():
        path = ROOT / name
        if path.name in ("ruff.toml", ".ruff.toml"):
            strays.append(name)
        elif path.name == "pyproject.toml" and path.parent != ROOT and "[tool.ruff" in path.read_text():
            strays.append(name)
    assert not strays, f"ruff is configured in the root pyproject.toml only: {strays}"

"""Every list of use cases agrees with the subpackages.

A use case is a subpackage optimaru/<use case>/ with a _core.pyi stub (setup.py
builds exactly those). CI reads that list off the repository; the lists that are
still written by hand are checked here, so a new use case cannot be left out of
one of them.
"""

from __future__ import annotations

import ast
import re
import subprocess
import tomllib
from pathlib import Path

import pytest

import optimaru

ROOT = Path(__file__).resolve().parent.parent
USE_CASES = {path.parent.name for path in (ROOT / "optimaru").glob("*/_core.pyi")}


def test_every_subpackage_is_a_use_case():
    assert len(USE_CASES) == 22
    assert all((ROOT / "optimaru" / name / "cpp" / "CMakeLists.txt").is_file() for name in USE_CASES)
    assert all((ROOT / "tests" / name).is_dir() for name in USE_CASES)


def test_a_use_case_keeps_only_its_sources_under_cpp():
    """What the extension and the core library build from. The C++ tests are in
    tests/<use case>/cpp/ and a C++ bench is in benchmarks/<slug>/cpp/. All four entries are
    required: setup.py compiles cpp/bindings.cpp and cpp/src/*.cpp against cpp/include."""
    tracked = subprocess.run(["git", "ls-files", "optimaru"], cwd=ROOT, capture_output=True, text=True)
    if tracked.returncode != 0:
        pytest.skip("not a git checkout")
    entries = {name: set() for name in USE_CASES}
    for path in tracked.stdout.splitlines():
        parts = path.split("/")
        if len(parts) > 3 and parts[1] in USE_CASES and parts[2] == "cpp":
            entries[parts[1]].add(parts[3])
    for name in sorted(USE_CASES):
        assert entries[name] == {"CMakeLists.txt", "bindings.cpp", "include", "src"}, name


def test_the_package_lists_every_use_case():
    assert set(optimaru.__all__) == USE_CASES


def test_the_contract_tests_solve_every_use_case():
    """tests/test_solve_contract.py solves one small problem per use case."""
    from ._problems import SMALL_PROBLEMS

    assert set(SMALL_PROBLEMS) == USE_CASES


def test_cmake_builds_every_use_case():
    text = (ROOT / "CMakeLists.txt").read_text()
    block = re.search(r"set\(OPTIMARU_ALL_USE_CASES\s+([^)]*)\)", text)
    assert block, "CMakeLists.txt: no OPTIMARU_ALL_USE_CASES"
    assert set(block.group(1).split()) == USE_CASES


def test_ci_reads_the_use_cases_off_the_repository():
    """ci.yml lists no use case by hand: its `use-cases` job globs the same stubs as USE_CASES
    above, and the three matrices take their lists from that job."""
    text = (ROOT / ".github" / "workflows" / "ci.yml").read_text()
    assert "for stub in optimaru/*/_core.pyi" in text
    assert text.count("fromJSON(needs.use-cases.outputs.names)") == 2
    assert text.count("fromJSON(needs.use-cases.outputs.benchmarks)") == 1
    # The one use case the CP-SAT matrix leaves out, as test_every_cpsat_benchmark_is_a_project does.
    assert 'select(. != "vehicle-routing")' in text
    assert not any(re.search(rf"^\s+- {name}$", text, re.M) for name in USE_CASES)


def test_every_cpsat_benchmark_is_a_project():
    """What the `benchmark-cpsat` matrix runs for each use case but vehicle routing (which has
    the OR-Tools Routing comparison in benchmarks/vehicle-routing itself)."""
    for name in USE_CASES - {"vehicle_routing"}:
        slug = name.replace("_", "-")
        project = ROOT / "benchmarks" / slug / "cpsat" / "pyproject.toml"
        assert project.is_file() or (ROOT / "benchmarks" / slug / "pyproject.toml").is_file(), slug


def test_every_benchmark_project_is_packaged_and_pins_one_external_solver():
    """uv installs a project, and so its `[project.scripts]` and its package for `uv run pytest`,
    only when it has a `[build-system]` table: without one `uv run <slug>-compare` cannot spawn,
    and CI does not notice, because it runs `python -m` from the project directory. The CP-SAT
    library is pinned by tag in every project that uses it (every use case but vehicle routing,
    compared with OR-Tools Routing, and school timetabling, which keeps its model here): one
    tag, moved in all of them, and the links into the library follow it. pytest is in the `dev`
    dependency group, which a plain `uv sync` installs (an extra or another group is not)."""
    slugs = {name.replace("_", "-") for name in USE_CASES}
    projects = {slug: ROOT / "benchmarks" / slug / "cpsat" / "pyproject.toml" for slug in slugs - {"vehicle-routing"}}
    projects["vehicle-routing"] = ROOT / "benchmarks" / "vehicle-routing" / "pyproject.toml"
    projects["cpsat-common"] = ROOT / "benchmarks" / "cpsat-common" / "pyproject.toml"
    tags = {}
    for slug, project in sorted(projects.items()):
        data = tomllib.loads(project.read_text())
        assert "build-system" in data, f"{slug}: uv will not install the package or its [project.scripts]"
        groups = data.get("dependency-groups", {})
        assert set(groups) == {"dev"}, f"{slug}: one dependency group, dev, which a plain uv sync installs"
        assert any(entry.startswith("pytest") for entry in groups["dev"]), f"{slug}: no pytest in the dev group"
        assert "optional-dependencies" not in data["project"], f"{slug}: test tools go in [dependency-groups]"
        source = data.get("tool", {}).get("uv", {}).get("sources", {}).get("external-solver")
        if source is not None:
            assert set(source) == {"git", "tag"}, f"{slug}: external-solver is pinned by tag, not {source}"
            tags[slug] = source["tag"]
    assert set(tags) == slugs - {"vehicle-routing", "school-timetabling"}
    assert len(set(tags.values())) == 1, f"the benchmark projects pin different external-solver tags: {tags}"
    (tag,) = set(tags.values())
    tracked = subprocess.run(["git", "ls-files", "*.md"], cwd=ROOT, capture_output=True, text=True)
    if tracked.returncode != 0:
        pytest.skip("not a git checkout")
    stale = {}
    for name in tracked.stdout.split():
        found = set(re.findall(r"optimaru/external-solver/blob/(v[\w.]+)/", (ROOT / name).read_text()))
        if found - {tag}:
            stale[name] = sorted(found - {tag})
    assert not stale, f"links into external-solver that are not on {tag}: {stale}"


def test_every_use_case_has_a_guide_and_a_benchmark():
    for name in USE_CASES:
        slug = name.replace("_", "-")
        assert (ROOT / "docs" / "use-cases" / slug / "README.md").is_file(), slug
        assert (ROOT / "docs" / "use-cases" / slug / "extending.md").is_file(), slug
        assert (ROOT / "benchmarks" / slug).is_dir(), slug


_NUMBER_WORDS = (
    "zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen "
    "seventeen eighteen nineteen twenty"
).split()
_TENS = {"twenty": 20, "thirty": 30, "forty": 40, "fifty": 50}


def _number(text: str) -> int | None:
    """`12`, `twelve` or `twenty-one` as a number."""
    text = text.lower()
    if text.isdigit():
        return int(text)
    if text in _NUMBER_WORDS:
        return _NUMBER_WORDS.index(text)
    tens, _, ones = text.partition("-")
    if tens in _TENS and (not ones or ones in _NUMBER_WORDS[1:10]):
        return _TENS[tens] + (_NUMBER_WORDS.index(ones) if ones else 0)
    return None


def _extending_guide(name: str) -> str:
    return (ROOT / "docs" / "use-cases" / name.replace("_", "-") / "extending.md").read_text()


def test_a_guide_states_the_number_of_constraints_its_use_case_has():
    """The first paragraph of docs/use-cases/<slug>/extending.md says "the model and its N
    constraints". N is what the specs module lists: `_DEFAULTS`, or employee scheduling's
    `SPEC_CLASSES`, which also holds the ones that are off by default. Read off the source, so
    it needs no compiled extension; a constraint added without the guide fails here."""
    for name in sorted(USE_CASES):
        opening = " ".join(" ".join(_extending_guide(name).split("\n")[:12]).split())
        stated = re.search(r"the model and its ([\w-]+) constraints are in", opening)
        assert stated, f"{name}: the guide's first paragraph does not say how many constraints the use case has"
        catalog = "SPEC_CLASSES" if name == "employee_scheduling" else "_DEFAULTS"
        listed = None
        for node in ast.parse((ROOT / "optimaru" / name / "constraints.py").read_text()).body:
            target = node.targets[0] if isinstance(node, ast.Assign) else getattr(node, "target", None)
            if getattr(target, "id", None) == catalog and isinstance(node.value, (ast.Tuple, ast.List)):
                listed = len(node.value.elts)
        assert listed is not None, f"{name}: no {catalog} tuple in constraints.py"
        assert _number(stated.group(1)) == listed, f"{name}: the guide says {stated.group(1)}, {catalog} lists {listed}"


def test_the_tests_a_guide_names_exist():
    """A guide names the checks a new constraint breaks, as `test_...` functions and as C++ test
    targets (`<ns>_plugin_test`). A renamed one would send the reader to nothing."""
    tracked = subprocess.run(["git", "ls-files", "*.py", "*CMakeLists.txt"], cwd=ROOT, capture_output=True, text=True)
    if tracked.returncode != 0:
        pytest.skip("not a git checkout")
    functions, targets = set(), set()
    for path in tracked.stdout.splitlines():
        text = (ROOT / path).read_text()
        if path.endswith(".py"):
            functions.update(re.findall(r"^\s*def (test_\w+)", text, re.M))
        else:
            targets.update(re.findall(r"add_executable\((\w+)", text))
    for name in sorted(USE_CASES):
        guide = _extending_guide(name)
        cited_functions = set(re.findall(r"`(test_\w+)`", guide))
        cited_targets = set(re.findall(r"`(\w+_test)`", guide))
        assert not cited_functions - functions, f"{name}: no such test: {sorted(cited_functions - functions)}"
        assert not cited_targets - targets, f"{name}: no such C++ test target: {sorted(cited_targets - targets)}"


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

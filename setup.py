"""Builds the `optimaru` wheel: every use case under use-cases/ as a subpackage.

Each use case keeps its own python/setup.py as the source of truth for what its
pybind11 extension compiles. This file runs those scripts with a recording stub
instead of setuptools, re-roots their paths at the repository root and renames
`<pkg>._core` to `optimaru.<subpackage>._core`. Nothing under use-cases/ is
modified.

    pip install .            # local build
    python -m build          # sdist + wheel (what the release workflow does)
"""

import os
import runpy
from contextlib import contextmanager

# setuptools must be imported before pybind11.setup_helpers: on Python 3.12+
# it provides the distutils shim that pybind11's helpers still import.
import setuptools
import pybind11.setup_helpers as helpers
from pybind11.setup_helpers import ParallelCompile, Pybind11Extension, build_ext

HERE = os.path.dirname(os.path.abspath(__file__))

# use-case directory → (subpackage under optimaru, import package inside python/)
USE_CASES = {
    "assembly-line-balancing": ("assembly_line_balancing", "assembly_line_balancing"),
    "bed-allocation": ("bed_allocation", "bed_allocation"),
    "bin-packing": ("bin_packing", "bin_packing"),
    "conference-scheduling": ("conference_scheduling", "conference_scheduling"),
    "employee-scheduling": ("employee_scheduling", "es_scheduling"),
    "facility-location": ("facility_location", "facility_location"),
    "flight-crew-scheduling": ("flight_crew_scheduling", "flight_crew"),
    "food-packaging": ("food_packaging", "food_packaging"),
    "knapsack": ("knapsack", "knapsack"),
    "maintenance-scheduling": ("maintenance_scheduling", "maintenance_scheduling"),
    "max-cut": ("max_cut", "max_cut"),
    "meeting-scheduling": ("meeting_scheduling", "meeting_scheduling"),
    "order-picking": ("order_picking", "order_picking"),
    "pickup-delivery": ("pickup_delivery", "pdp_routing"),
    "project-job-scheduling": ("project_job_scheduling", "project_job"),
    "quadratic-assignment": ("quadratic_assignment", "quadratic_assignment"),
    "school-timetabling": ("school_timetabling", "st_timetabling"),
    "semiconductor-scheduling": ("semiconductor_scheduling", "semiconductor_scheduling"),
    "sports-league-scheduling": ("sports_league_scheduling", "sports_league"),
    "task-assigning": ("task_assigning", "task_assigning"),
    "tournament-scheduling": ("tournament_scheduling", "tournament_scheduling"),
    "vehicle-routing": ("vehicle_routing", "vrp_routing"),
}


@contextmanager
def recording():
    """Swap setuptools.setup / Pybind11Extension for recorders while a use-case setup.py runs."""
    calls = []

    def fake_extension(name, sources, **kwargs):
        calls.append((name, list(sources), kwargs))

    saved = setuptools.setup, helpers.Pybind11Extension
    setuptools.setup = lambda **_: None
    helpers.Pybind11Extension = fake_extension
    try:
        yield calls
    finally:
        setuptools.setup, helpers.Pybind11Extension = saved


def use_case_extensions(slug, subpackage, import_pkg):
    py_dir = os.path.join("use-cases", slug, "python")
    script = os.path.join(HERE, py_dir, "setup.py")
    cwd = os.getcwd()
    with recording() as calls:
        # Their paths are relative to python/; run from there so any os.path
        # checks they do resolve the same way as a standalone build.
        os.chdir(os.path.join(HERE, py_dir))
        try:
            runpy.run_path(script, run_name="__main__")
        finally:
            os.chdir(cwd)

    rebase = lambda p: os.path.normpath(os.path.join(py_dir, p)).replace(os.sep, "/")
    extensions = []
    for name, sources, kwargs in calls:
        assert name.startswith(import_pkg + "."), f"{slug}: unexpected extension name {name}"
        kwargs = dict(kwargs)
        kwargs["include_dirs"] = [rebase(d) for d in kwargs.get("include_dirs", [])]
        extensions.append(
            Pybind11Extension(
                f"optimaru.{subpackage}{name[len(import_pkg):]}",
                sources=[rebase(s) for s in sources],
                **kwargs,
            )
        )
    return extensions


def subpackages():
    packages, package_dir = ["optimaru"], {"optimaru": "python/optimaru"}
    for slug, (subpackage, import_pkg) in USE_CASES.items():
        src = f"use-cases/{slug}/python/{import_pkg}"
        package_dir[f"optimaru.{subpackage}"] = src
        packages.append(f"optimaru.{subpackage}")
        for nested in setuptools.find_packages(os.path.join(HERE, src)):
            packages.append(f"optimaru.{subpackage}.{nested}")
    return packages, package_dir


# 15 extensions — compile their translation units in parallel.
ParallelCompile("OPTIMARU_BUILD_JOBS", default=0).install()

ext_modules = [
    ext
    for slug, (subpackage, import_pkg) in USE_CASES.items()
    for ext in use_case_extensions(slug, subpackage, import_pkg)
]
packages, package_dir = subpackages()

setuptools.setup(
    packages=packages,
    package_dir=package_dir,
    package_data={pkg: ["*.pyi", "py.typed"] for pkg in packages},
    ext_modules=ext_modules,
    cmdclass={"build_ext": build_ext},
)

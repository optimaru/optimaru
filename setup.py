"""Builds the `optimaru` wheel: the Python package in optimaru/ and one pybind11
extension, `optimaru.<use case>._core`, per use case.

Every extension compiles its use case's bindings (optimaru/<use case>/cpp/bindings.cpp)
and C++ sources (optimaru/<use case>/cpp/src/) against the header-only engine
(optimaru/cpp/optimaru/).

    pip install .            # local build
    uv sync                  # editable build into .venv (extensions built in place)
    python -m build          # sdist + wheel (what the release workflow does)

OPTIMARU_USE_CASES=knapsack,vehicle_routing builds only those extensions; CI uses
it to test one use case per job. Leave it unset for a release build.
"""

import os

# setuptools must be imported before pybind11.setup_helpers: on Python 3.12+
# it provides the distutils shim that pybind11's helpers still import.
import setuptools
from pybind11.setup_helpers import ParallelCompile, Pybind11Extension, build_ext

ENGINE_INCLUDE = "optimaru/cpp"

# use case → the C++ sources under optimaru/<use case>/cpp/src/ its extension compiles.
USE_CASES = {
    "assembly_line_balancing": ["score_calculator", "incremental_calculator", "construction", "solver"],
    "bed_allocation": ["score_calculator", "incremental_calculator", "solver"],
    "bin_packing": ["score_calculator", "incremental_calculator", "construction", "solver"],
    "conference_scheduling": ["score_calculator", "incremental_calculator", "solver"],
    "employee_scheduling": ["score_calculator", "incremental_calculator", "solver"],
    "facility_location": ["score_calculator", "incremental_calculator", "solver"],
    "flight_crew_scheduling": ["score_calculator", "incremental_calculator", "solver"],
    "food_packaging": ["score_calculator", "incremental_calculator", "solver", "demo_data"],
    "knapsack": ["score_calculator", "incremental_calculator", "construction", "solver"],
    "maintenance_scheduling": ["score_calculator", "incremental_calculator", "solver", "demo_data"],
    "max_cut": ["score_calculator", "incremental_calculator", "solver"],
    "meeting_scheduling": ["score_calculator", "incremental_calculator", "solver"],
    "order_picking": ["score_calculator", "incremental_calculator", "solver", "demo_data"],
    "pickup_delivery": ["score_calculator", "incremental_calculator", "haversine", "solver", "demo_data"],
    "project_job_scheduling": ["score_calculator", "incremental_calculator", "solver"],
    "quadratic_assignment": ["score_calculator", "incremental_calculator", "solver"],
    "school_timetabling": [
        "score_calculator",
        "incremental_calculator",
        "solver",
        "canonical_score_calculator",
        "canonical_incremental_calculator",
        "canonical_solver",
    ],
    "semiconductor_scheduling": [
        "decoder",
        "score_calculator",
        "evaluator",
        "incremental_calculator",
        "solver",
    ],
    "sports_league_scheduling": ["score_calculator", "incremental_calculator", "solver"],
    "task_assigning": ["score_calculator", "incremental_calculator", "solver"],
    "tournament_scheduling": ["score_calculator", "incremental_calculator", "solver"],
    "vehicle_routing": ["score_calculator", "incremental_calculator", "haversine", "solver"],
}


def selected_use_cases():
    wanted = os.environ.get("OPTIMARU_USE_CASES", "").replace(",", " ").split()
    unknown = sorted(set(wanted) - set(USE_CASES))
    if unknown:
        raise SystemExit(f"OPTIMARU_USE_CASES: unknown use case(s) {unknown}; known: {sorted(USE_CASES)}")
    return [name for name in USE_CASES if not wanted or name in wanted]


def extension(name):
    cpp = f"optimaru/{name}/cpp"
    return Pybind11Extension(
        f"optimaru.{name}._core",
        sources=[f"{cpp}/bindings.cpp", *(f"{cpp}/src/{source}.cpp" for source in USE_CASES[name])],
        include_dirs=[f"{cpp}/include", ENGINE_INCLUDE],
        cxx_std=17,
    )


# Compile every extension's translation units in parallel.
ParallelCompile("OPTIMARU_BUILD_JOBS", default=0).install()

packages = ["optimaru", *(f"optimaru.{name}" for name in USE_CASES)]

setuptools.setup(
    packages=packages,
    package_data={pkg: ["*.pyi", "py.typed"] for pkg in packages},
    ext_modules=[extension(name) for name in selected_use_cases()],
    cmdclass={"build_ext": build_ext},
)

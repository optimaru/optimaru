"""Builds the `optimaru` wheel: the Python package in optimaru/ and one pybind11
extension, `optimaru.<use case>._core`, per use case.

Every extension compiles its use case's bindings (optimaru/<use case>/cpp/bindings.cpp)
and the C++ sources in optimaru/<use case>/cpp/src/ (all but main.cpp, the demo)
against the header-only engine (optimaru/cpp/optimaru/).

    pip install .            # local build
    uv sync                  # editable build into .venv (extensions built in place)
    python -m build          # sdist + wheel (what the release workflow does)

OPTIMARU_USE_CASES=knapsack,vehicle_routing builds only those extensions; CI uses
it to test one use case per job. Leave it unset for a release build.
"""

import os
from pathlib import Path

# setuptools must be imported before pybind11.setup_helpers: on Python 3.12+
# it provides the distutils shim that pybind11's helpers still import.
import setuptools
from pybind11.setup_helpers import ParallelCompile, Pybind11Extension, build_ext

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE_INCLUDE = "optimaru/cpp"

# Every use case: a subpackage optimaru/<use case>/ with a _core.pyi stub and its C++ in cpp/.
USE_CASES = sorted(p.parent.name for p in Path(HERE, "optimaru").glob("*/_core.pyi"))


def selected_use_cases():
    wanted = os.environ.get("OPTIMARU_USE_CASES", "").replace(",", " ").split()
    unknown = sorted(set(wanted) - set(USE_CASES))
    if unknown:
        raise SystemExit(f"OPTIMARU_USE_CASES: unknown use case(s) {unknown}; known: {sorted(USE_CASES)}")
    return [name for name in USE_CASES if not wanted or name in wanted]


def sources(name):
    """bindings.cpp and every cpp/src/*.cpp except main.cpp (the demo): the same core
    library the use case's CMakeLists.txt builds."""
    cpp = f"optimaru/{name}/cpp"
    src = sorted(p.name for p in Path(HERE, cpp, "src").glob("*.cpp") if p.name != "main.cpp")
    return [f"{cpp}/bindings.cpp", *(f"{cpp}/src/{file}" for file in src)]


def extension(name):
    cpp = f"optimaru/{name}/cpp"
    return Pybind11Extension(
        f"optimaru.{name}._core",
        sources=sources(name),
        include_dirs=[f"{cpp}/include", ENGINE_INCLUDE],
        cxx_std=17,
    )


# Compile every extension's translation units in parallel.
ParallelCompile("OPTIMARU_BUILD_JOBS", default=0).install()

packages = setuptools.find_packages(HERE, include=["optimaru", "optimaru.*"])

setuptools.setup(
    packages=packages,
    package_data={pkg: ["*.pyi", "py.typed"] for pkg in packages},
    ext_modules=[extension(name) for name in selected_use_cases()],
    cmdclass={"build_ext": build_ext},
)

"""Builds the `flight_crew` package: the pybind11 extension
`flight_crew._core` compiled together with the flight-crew-scheduling C++
domain wrapper in ../cpp, importing the generic optimaru engine headers from
../../../include without modifying them, plus the pure-Python helpers in
flight_crew/.

Local dev:   uv sync --all-groups
or:          uv run python setup.py build_ext --inplace
"""

import os

from pybind11.setup_helpers import Pybind11Extension, build_ext

# setuptools must be imported before pybind11.setup_helpers: on Python 3.12+
# it provides the distutils shim that pybind11's helpers still import.
from setuptools import setup

CORE = os.path.join("..", "cpp")
ENGINE = os.path.join("..", "..", "..")

ext_modules = [
    Pybind11Extension(
        "flight_crew._core",
        sources=[
            "bindings.cpp",
            os.path.join(CORE, "src", "score_calculator.cpp"),
            os.path.join(CORE, "src", "incremental_calculator.cpp"),
            os.path.join(CORE, "src", "solver.cpp"),
        ],
        include_dirs=[
            os.path.join(CORE, "include"),
            os.path.join(ENGINE, "include"),  # header-only generic engine
        ],
        cxx_std=17,
    )
]

# Project metadata, packages and package_data live in pyproject.toml; this
# file only wires up the pybind11 extension build.
setup(
    ext_modules=ext_modules,
    cmdclass={"build_ext": build_ext},
)

"""Builds the `pdp_routing` package: the pybind11 extension `pdp_routing._core`
compiled together with the C++ use case in ../cpp (linked, not modified).

Local dev:   uv sync --all-groups
or:          uv run python setup.py build_ext --inplace
"""

import os

# setuptools must be imported before pybind11.setup_helpers: on Python 3.12+
# it provides the distutils shim that pybind11's helpers still import.
from setuptools import setup

from pybind11.setup_helpers import Pybind11Extension, build_ext

CORE = os.path.join("..", "cpp")
# The header-only generic engine (include/optimaru/), consumed as an external
# header-only dependency.
ENGINE = os.path.join("..", "..", "..", "include")

ext_modules = [
    Pybind11Extension(
        "pdp_routing._core",
        sources=[
            "bindings.cpp",
            os.path.join(CORE, "src", "score_calculator.cpp"),
            os.path.join(CORE, "src", "incremental_calculator.cpp"),
            os.path.join(CORE, "src", "haversine.cpp"),
            os.path.join(CORE, "src", "solver.cpp"),
            os.path.join(CORE, "src", "demo_data.cpp"),
        ],
        include_dirs=[os.path.join(CORE, "include"), ENGINE],
        cxx_std=17,
    )
]

setup(ext_modules=ext_modules, cmdclass={"build_ext": build_ext})

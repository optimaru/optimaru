"""Builds the `order_picking` package: the pybind11 extension
`order_picking._core` compiled together with the order-picking C++ use case
in ../cpp (linked, not modified), importing the generic optimaru engine
headers from ../../../include without modifying them.

Local dev:   uv sync --all-groups
or:          uv run python setup.py build_ext --inplace
"""

import os

from pybind11.setup_helpers import Pybind11Extension, build_ext
from setuptools import setup

CORE = os.path.join("..", "cpp")
ENGINE = os.path.join("..", "..", "..")

ext_modules = [
    Pybind11Extension(
        "order_picking._core",
        sources=[
            "bindings.cpp",
            os.path.join(CORE, "src", "score_calculator.cpp"),
            os.path.join(CORE, "src", "incremental_calculator.cpp"),
            os.path.join(CORE, "src", "solver.cpp"),
            os.path.join(CORE, "src", "demo_data.cpp"),
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
setup(ext_modules=ext_modules, cmdclass={"build_ext": build_ext})

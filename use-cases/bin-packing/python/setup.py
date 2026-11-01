"""Builds the `bin_packing` package: the pybind11 extension `bin_packing._core`
compiled together with the bin-packing C++ domain in ../cpp, importing the
generic optimaru engine headers from ../../../include without modifying them,
plus the Python helpers in bin_packing/.

Local dev:   uv sync --all-groups --all-extras
or:          uv run python setup.py build_ext --inplace
"""

import os

from pybind11.setup_helpers import Pybind11Extension, build_ext
from setuptools import setup

CORE = os.path.join("..", "cpp")
ENGINE = os.path.join("..", "..", "..")

ext_modules = [
    Pybind11Extension(
        "bin_packing._core",
        sources=[
            "bindings.cpp",
            os.path.join(CORE, "src", "score_calculator.cpp"),
            os.path.join(CORE, "src", "incremental_calculator.cpp"),
            os.path.join(CORE, "src", "construction.cpp"),
            os.path.join(CORE, "src", "solver.cpp"),
        ],
        include_dirs=[
            os.path.join(CORE, "include"),
            os.path.join(ENGINE, "include"),  # header-only generic engine
        ],
        cxx_std=17,
    )
]


class BuildExt(build_ext):
    """build_ext that keeps every object file inside this build's temp directory.

    distutils writes an object to <build_temp>/<source path>, so ../cpp/src/*.cpp
    would land one level ABOVE build_temp. An editable build's build_temp is a
    fresh $TMPDIR/<random>.build-temp, so every use case would share
    $TMPDIR/cpp/src/ and concurrent builds could link each other's solver.o.
    Nesting build_temp one level per "../" keeps those objects in a directory
    this build owns. (The top-level setup.py records only the extensions and
    builds them from root-relative paths, so it does not need this.)
    """

    def finalize_options(self):
        super().finalize_options()
        sources = [os.path.normpath(s) for e in self.extensions for s in e.sources]
        up = max((s.split(os.sep).count(os.pardir) for s in sources), default=0)
        self.build_temp = os.path.join(self.build_temp, *(["objects"] * up))


# Project metadata, packages and package_data live in pyproject.toml; this
# file only wires up the pybind11 extension build.
setup(
    ext_modules=ext_modules,
    cmdclass={"build_ext": BuildExt},
)

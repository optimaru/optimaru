"""Builds the `vrp_routing` package: the pybind11 extension `vrp_routing._core`
compiled together with the existing C++ use case in ../cpp (linked,
not modified).

Local dev:   uv sync --all-groups
or:          uv run python setup.py build_ext --inplace
"""

import os

# setuptools must be imported before pybind11.setup_helpers: on Python 3.12+
# it provides the distutils shim that pybind11's helpers still import.
from setuptools import setup

from pybind11.setup_helpers import Pybind11Extension, build_ext

CORE = os.path.join("..", "cpp")
# The header-only generic engine (include/), consumed as an external
# header-only dependency.
ENGINE = os.path.join("..", "..", "..", "include")

ext_modules = [
    Pybind11Extension(
        "vrp_routing._core",
        sources=[
            "bindings.cpp",
            os.path.join(CORE, "src", "score_calculator.cpp"),
            os.path.join(CORE, "src", "incremental_calculator.cpp"),
            os.path.join(CORE, "src", "haversine.cpp"),
            os.path.join(CORE, "src", "solver.cpp"),
        ],
        include_dirs=[os.path.join(CORE, "include"), ENGINE],
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


setup(ext_modules=ext_modules, cmdclass={"build_ext": BuildExt})

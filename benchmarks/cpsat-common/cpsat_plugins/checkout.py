"""Check that the benchmarks run external-solver against the engine in this checkout.

The CP-SAT models live in external-solver (github.com/optimaru/external-solver)
and import ``optimaru.<use case>``. Every benchmark project installs ``optimaru``
from this checkout, as an editable path dependency that overrides
external-solver's pin on the published release, so both solvers build, select
and score with the same modules: the same constraint classes, models and
``constraint_costs``. ``use_checkout_engine`` fails fast when
``optimaru.<use case>`` comes from anywhere else, for example a published wheel
left in the environment. It also sets an API key in the environment aside: with
``OPTIMARU_API_KEY`` the engine's side would be solved on the optimaru API, and a
benchmark measures this machine.
"""

from __future__ import annotations

import importlib
import os
import sys
from pathlib import Path
from types import ModuleType

#: The checkout: benchmarks/cpsat-common/cpsat_plugins/checkout.py -> repository root.
REPO_ROOT = Path(__file__).resolve().parents[3]


def use_checkout_engine(subpackage: str, repo_root: Path = REPO_ROOT) -> ModuleType:
    """Import ``optimaru.<subpackage>`` and check it is this checkout's. Idempotent.

    Raises RuntimeError if the module is loaded from outside ``repo_root``, since
    the benchmark would then measure another engine. Removes ``OPTIMARU_API_KEY``
    from the environment of this process and the ones it starts, and calls
    ``optimaru.use_local()``, since the engine would otherwise solve on the
    optimaru API instead of here.
    """
    os.environ.pop("OPTIMARU_API_KEY", None)
    module = importlib.import_module(f"optimaru.{subpackage}")
    use_local = getattr(sys.modules["optimaru"], "use_local", None)
    if use_local is not None:  # and a key given in code: an engine before the setting has neither
        use_local()
    location = Path(module.__file__).resolve()
    if repo_root.resolve() not in location.parents:
        raise RuntimeError(
            f"optimaru.{subpackage} is imported from {location}, not from the checkout at "
            f"{repo_root}; install the benchmark project with `uv sync` so that optimaru "
            "comes from this repository"
        )
    return module

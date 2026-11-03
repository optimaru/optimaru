"""Shared pieces of the engine vs CP-SAT benchmarks.

The CP-SAT models and their constraint-plugin contract live in external-solver
(github.com/optimaru/external-solver). This package holds the comparison runner
(``benchmark.run_cli``) and ``checkout.use_checkout_engine``, which points those
models at the engine in this checkout.
"""

from .checkout import use_checkout_engine

__all__ = ["use_checkout_engine"]

"""Engine vs CP-SAT comparison for bin-packing.

The CP-SAT model is ``external_solver.bin_packing`` (github.com/optimaru/external-solver).
Importing this package checks that ``optimaru.bin_packing`` is the engine in this
checkout, so both solvers use the same modules (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("bin_packing")

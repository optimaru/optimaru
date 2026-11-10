"""Engine vs CP-SAT comparison for assembly-line-balancing.

The CP-SAT model is ``external_solver.assembly_line_balancing`` (github.com/optimaru/external-solver).
Importing this package checks that ``optimaru.assembly_line_balancing`` is the engine in this
checkout, so both solvers use the same modules (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("assembly_line_balancing")

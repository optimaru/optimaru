"""Engine vs CP-SAT comparison for quadratic-assignment.

The CP-SAT model is ``external_solver.quadratic_assignment`` (github.com/optimaru/external-solver).
Importing this package checks that ``optimaru.quadratic_assignment`` is the engine in this
checkout, so both solvers use the same modules (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("quadratic_assignment")

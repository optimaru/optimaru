"""Engine vs CP-SAT comparison for conference-scheduling.

The CP-SAT model is ``external_solver.conference_scheduling`` (github.com/optimaru/external-solver).
Importing this package checks that ``optimaru.conference_scheduling`` is the engine in this
checkout, so both solvers use the same modules (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("conference_scheduling")

"""Engine vs CP-SAT comparison for tournament-scheduling.

The CP-SAT model is ``external_solver.tournament_scheduling`` (github.com/optimaru/external-solver).
Importing this package checks that ``optimaru.tournament_scheduling`` is the engine in this
checkout, so both solvers use the same modules (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("tournament_scheduling")

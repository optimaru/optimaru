"""Engine vs CP-SAT comparison for pickup-delivery.

The CP-SAT model is :mod:`pickup_delivery_cpsat.model`: external-solver v0.3.0's
``external_solver.pickup_delivery``, kept here and ported to the optimaru 0.6 input.
Importing this package checks that ``optimaru.pickup_delivery`` is the engine in this
checkout, so both solvers use the same modules (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("pickup_delivery")

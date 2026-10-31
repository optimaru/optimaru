# conference-scheduling-cpsat

CP-SAT alternative exposing the conference constraint provider as a selectable
catalog. Passing `constraints` replaces the default catalog.

Each plugin subclasses the engine's spec (`conference_scheduling.constraints`) and only
builds its CP-SAT terms; the default levels and weights, the reward flags and the
flippability are read from `conference_scheduling.constraint_info()` (the C++
components), so `catalog()` equals the engine catalog. The three `*_diversity_cost`
rewards refuse `hard=True`. As on every CP-SAT reference, `exponent != 1` and
non-integer weights are not supported.

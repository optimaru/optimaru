# conference-scheduling-cpsat

CP-SAT alternative exposing the conference constraint provider as a selectable
catalog. Passing `constraints` replaces the default catalog.

Each plugin subclasses the engine's spec (`conference_scheduling.constraints`) and only
builds its CP-SAT terms; the default levels and weights, the reward flags and the
flippability are read from `conference_scheduling.constraint_info()` (the C++
components), so `catalog()` equals the engine catalog. The three `*_diversity_cost`
rewards refuse `level="hard"`. As on every CP-SAT reference, `exponent != 1` and
non-integer weights are not supported.

Since 0.3.0 a constraint may be picked several times (one set of terms per pick, each
with its own knobs, its `level` included) and takes integral `offset` / `clip`: every
conference constraint is global, so each plugin sums its pieces into the one total the
engine shapes and adds it once (`terms_per_unit`), a total of 0 included. A fractional
offset is refused ("offset must be an integer for CP-SAT"), and so is `level="medium"`
(this use case has no medium level), as on the engine.

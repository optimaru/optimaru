"""Engine vs CP-SAT comparison for school-timetabling.

Unlike the other use cases, the CP-SAT reference of the canonical model is ``teacher_scheduling``,
the other package of this project. ``benchmark`` runs the three tracks
(``school-timetabling-compare``); ``evaluate``, ``cpsat_quickstart`` and ``gen_canonical`` are
its track-1 evaluator, track-1 CP-SAT model and track-2 instance generator, each also runnable
as ``python -m school_timetabling_cpsat.<module>``. Importing this package imports neither the
engine nor OR-Tools, so track 1 runs without the engine extension.
"""

"""Constraint spot checks: re-verifies the hand-computed cases of the C++ tests through _core."""

import pytest

import food_packaging as fp

BIG = 1_000_000_000
PA_S, PA_L, PB = 0, 1, 2  # the three products of the Java test (A large keeps the matrix shape)


def make_schedule(operator_count, lines, loose_jobs=(), pooled_jobs=()):
    """Build a schedule with the product/cleaning matrix of Java ConstraintProviderTest.

    lines: [(operator_index, [(product, duration, ideal_end, max_end), ...])]
    """
    products = [fp.Product("1", "Product A small"), fp.Product("2", "Product A large"), fp.Product("3", "Product B")]
    cleanup = [
        0, 0, 40,  # previous: A small
        5, 0, 40,  # previous: A large
        60, 60, 0,  # previous: B
    ]  # fmt: skip
    jobs = []
    core_lines = []

    def add_job(spec):
        product, duration, ideal, max_end = spec
        index = len(jobs)
        jobs.append(fp.Job(str(index), f"job{index}", product, duration, 0, ideal, max_end))
        return index

    for operator_index, specs in lines:
        indices = [add_job(spec) for spec in specs]
        core_lines.append(fp.Line(str(len(core_lines)), f"Line {len(core_lines)}", 0, operator_index, indices))
    for spec in loose_jobs:
        add_job(spec)
    pool = [add_job(spec) for spec in pooled_jobs]
    operators = [fp.Operator(f"Operator {o + 1}") for o in range(operator_count)]
    return fp.PackagingSchedule(0, 14 * 24 * 60, products, cleanup, operators, core_lines, jobs, pool)


def test_max_end_date_time():
    # J1 fits 200 exactly; J2 ends at 350, 100 minutes late against maxEnd 250.
    schedule = make_schedule(1, [(0, [(PA_S, 200, BIG, 200), (PA_S, 150, BIG, 250)])])
    assert fp.calculate_score(schedule) == fp.Score(-100, 0, -(350 * 350))
    assert fp.analyze(schedule).max_end_date_time == 100


def test_cleanup_chain_shadows():
    # B -> A small cleaning 60 minutes: J1 cleans [100,160) then produces 160..260.
    schedule = make_schedule(1, [(0, [(PB, 100, BIG, BIG), (PA_S, 100, BIG, BIG)])])
    schedule.update_shadow_variables()
    j1 = schedule.jobs[1]
    assert j1.line_index == 0
    assert (j1.start_cleaning_minute, j1.start_production_minute, j1.end_minute) == (100, 160, 260)
    j0 = schedule.jobs[0]
    assert (j0.start_cleaning_minute, j0.start_production_minute, j0.end_minute) == (0, 0, 100)


def test_operator_cleaning_conflict():
    # Lines 0/1 share operator 0: cleaning windows [10,70) and [30,90) overlap 40 minutes.
    schedule = make_schedule(
        2,
        [
            (0, [(PB, 10, BIG, BIG), (PA_S, 100, BIG, BIG)]),
            (0, [(PB, 30, BIG, BIG), (PA_S, 100, BIG, BIG)]),
            (1, [(PB, 5, BIG, BIG), (PA_S, 100, BIG, BIG)]),
        ],
    )
    breakdown = fp.analyze(schedule)
    assert breakdown.operator_cleaning_conflict == 40
    assert breakdown.minimize_makespan == 170 * 170 + 190 * 190 + 165 * 165


def test_maximize_jobs_assigned():
    # Both loose jobs (outside any list) and jobs in the pool count as unassigned.
    schedule = make_schedule(
        1,
        [(0, [(PA_S, 100, BIG, BIG)])],
        loose_jobs=[(PA_S, 200, BIG, BIG)],
        pooled_jobs=[(PB, 50, BIG, BIG)],
    )
    assert fp.calculate_score(schedule) == fp.Score(0, -250, -(100 * 100))


def test_minimize_makespan():
    # Line 0: 100 minutes. Line 1: ends at 200 + cleaning 40 + 1000 = 1240.
    schedule = make_schedule(
        2,
        [(0, [(PA_S, 100, BIG, BIG)]), (1, [(PA_S, 200, BIG, BIG), (PB, 1000, BIG, BIG)])],
        loose_jobs=[(PA_S, 6000, BIG, BIG)],
    )
    assert fp.calculate_score(schedule) == fp.Score(0, -6000, -(100 * 100 + 1240 * 1240))


def test_missing_cleanup_raises_like_java():
    schedule = make_schedule(1, [(0, [(PB, 100, BIG, BIG), (PA_S, 100, BIG, BIG)])])
    cleanup = schedule.cleanup_minutes
    cleanup[PB * 3 + PA_S] = -1  # remove the entry (Java: key missing from the map)
    schedule.cleanup_minutes = cleanup
    with pytest.raises(ValueError, match="Cleanup duration"):
        fp.calculate_score(schedule)


def test_operator_helpers():
    # The shared cleaning windows of lines 0/1 overlap 58 minutes; hill climbing separates them to 0.
    schedule = make_schedule(
        2,
        [
            (0, [(PB, 10, BIG, BIG), (PA_S, 100, BIG, BIG)]),
            (0, [(PB, 12, BIG, BIG), (PA_S, 100, BIG, BIG)]),
            (1, [(PB, 300, BIG, BIG), (PA_S, 100, BIG, BIG)]),
        ],
    )
    assert fp.analyze(schedule).operator_cleaning_conflict == 58
    assert fp.improve_operator_assignment(schedule) is True
    assert fp.analyze(schedule).operator_cleaning_conflict == 0

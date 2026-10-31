"""Every constraint's raw cost, hand-calculated: a positive and a zero case each.

`constraint_costs(schedule, [spec])` reports the RAW per-employee costs (one
element for a global constraint), before weight, exponent and level; 0 means
no violation (and nothing earned for the two rewards). The rosters are tiny
and every number below is worked out in the comment next to it.
"""

import math

import pytest

import es_scheduling as es
from es_scheduling.constraints import (
    AssignmentCost,
    DayPatternCost,
    DesiredDayCost,
    DesiredShiftCost,
    EmployeeUnavailabilityCost,
    LocationImbalanceCost,
    LocationRunGapCost,
    LocationRunLengthCost,
    MaxConsecutiveDaysCost,
    MaxConsecutiveDaysOffCost,
    MaxOneShiftPerDayCost,
    MaxWeeklyMinutesCost,
    MinConsecutiveDaysCost,
    MinRestCost,
    MinWeeklyMinutesCost,
    MinWeeklyRestDaysCost,
    RequiredSkillCost,
    RestAfterConsecutiveWorkCost,
    RestAfterLocationRunCost,
    ShiftCountGapCost,
    ShiftCountImbalanceCost,
    ShiftCoverageCost,
    ShiftOverlapCost,
    TargetDaysOffCost,
    UnassignedShiftsCost,
    UndesiredDayCost,
    WeeklyMinutesGapCost,
    WorkerDayCountInRangeCost,
    WorkerLocationCountCost,
    WorkerPairingCost,
)


def roster(employees, shifts):
    """employees: es.Employee list; shifts: (day, start hour, hours, location, skill, employee) tuples."""
    built = []
    for index, (day, hour, hours, location, skill, employee) in enumerate(shifts):
        start = es.minutes(day, hour)
        shift = es.Shift(f"S{index}", start, start + hours * 60, location, skill)
        shift.employee_index = employee
        built.append(shift)
    return es.EmployeeSchedule(employees, built)


def workers(count, **fields):
    return [es.Employee(f"E{index}", **fields) for index in range(count)]


def days(employee, day_list, location="L", hour=9, hours=8):
    return [(day, hour, hours, location, "", employee) for day in day_list]


def raw(schedule, spec):
    """The raw costs of the one constraint `spec`, checked to be the only entry."""
    costs = es.constraint_costs(schedule, [spec])
    assert list(costs) == [spec.name]
    return costs[spec.name]["costs"]


def test_required_skill_cost():
    # E0 holds A; it works a shift needing B (1) and one needing A (0).
    shifts = [(0, 9, 8, "L", "B", 0), (1, 9, 8, "L", "A", 0)]
    assert raw(roster([es.Employee("E0", {"A"})], shifts), RequiredSkillCost()) == [1.0]
    assert raw(roster([es.Employee("E0", {"A", "B"})], shifts), RequiredSkillCost()) == [0.0]


def test_employee_unavailability_cost():
    # An 8-hour shift on the unavailable day 1: 480 minutes.
    shifts = days(0, [1])
    assert raw(roster([es.Employee("E0", unavailable_dates={1})], shifts), EmployeeUnavailabilityCost()) == [480.0]
    assert raw(roster([es.Employee("E0", unavailable_dates={2})], shifts), EmployeeUnavailabilityCost()) == [0.0]


def test_shift_overlap_cost():
    # 08-12 and 11-13 overlap for 60 minutes; 08-10 and 12-14 do not.
    overlapping = [(0, 8, 4, "L", "", 0), (0, 11, 2, "L", "", 0)]
    apart = [(0, 8, 2, "L", "", 0), (0, 12, 2, "L", "", 0)]
    assert raw(roster(workers(1), overlapping), ShiftOverlapCost()) == [60.0]
    assert raw(roster(workers(1), apart), ShiftOverlapCost()) == [0.0]


def test_max_one_shift_per_day_cost():
    # Three shifts on day 0 are three pairs; one shift per day is none.
    same_day = [(0, hour, 2, "L", "", 0) for hour in (8, 12, 16)]
    assert raw(roster(workers(1), same_day), MaxOneShiftPerDayCost()) == [3.0]
    assert raw(roster(workers(1), days(0, [0, 1, 2])), MaxOneShiftPerDayCost()) == [0.0]


def test_min_rest_cost():
    # Day 0 20:00-22:00, day 1 03:00-05:00: 300 minutes of rest, 600 - 300 short.
    short_rest = [(0, 20, 2, "L", "", 0), (1, 3, 2, "L", "", 0)]
    assert raw(roster(workers(1), short_rest), MinRestCost()) == [300.0]
    # Two 09-17 shifts on consecutive days rest 960 minutes: fine at 600, 40 short at 1000.
    assert raw(roster(workers(1), days(0, [0, 1])), MinRestCost()) == [0.0]
    assert raw(roster(workers(1), days(0, [0, 1])), MinRestCost(min_gap_minutes=1000)) == [40.0]


def test_undesired_day_cost():
    shifts = days(0, [0])
    undesired = [es.Employee("E0", undesired_dates={0})]
    assert raw(roster(undesired, shifts), UndesiredDayCost()) == [480.0]
    assert raw(roster(undesired, shifts), UndesiredDayCost(per_day=True)) == [1.0]
    assert raw(roster([es.Employee("E0", undesired_dates={1})], shifts), UndesiredDayCost()) == [0.0]


def test_desired_day_cost_is_a_reward():
    shifts = days(0, [0])
    desired = [es.Employee("E0", desired_dates={0})]
    assert raw(roster(desired, shifts), DesiredDayCost()) == [-480.0]
    assert raw(roster(desired, shifts), DesiredDayCost(per_day=True)) == [-1.0]
    assert raw(roster([es.Employee("E0", desired_dates={3})], shifts), DesiredDayCost()) == [0.0]


def test_desired_shift_cost_is_a_reward():
    employee = es.Employee("E0")
    employee.desired_shifts = {(0, "L")}
    assert raw(roster([employee], days(0, [0], "L")), DesiredShiftCost()) == [-1.0]
    assert raw(roster([employee], days(0, [0], "M")), DesiredShiftCost()) == [0.0]  # another location


def test_shift_count_imbalance_cost():
    # Counts [2, 1, 1]: sqrt(6 - 16/3) = 0.816497 (six significant figures).
    uneven = days(0, [0, 3]) + days(1, [6]) + days(2, [9])
    assert raw(roster(workers(3), uneven), ShiftCountImbalanceCost()) == [pytest.approx(0.816497, abs=1e-12)]
    even = days(0, [0]) + days(1, [3]) + days(2, [6])
    assert raw(roster(workers(3), even), ShiftCountImbalanceCost()) == [0.0]


def test_max_consecutive_days_cost():
    # Four days in a row against a cap of 2: 2 too many; 2 + 2 days with a gap: none.
    assert raw(roster(workers(1), days(0, [0, 1, 2, 3])), MaxConsecutiveDaysCost(value=2)) == [2.0]
    assert raw(roster(workers(1), days(0, [0, 1, 3, 4])), MaxConsecutiveDaysCost(value=2)) == [0.0]


def test_min_consecutive_days_cost():
    # Runs of 1 (day 0) and 3 (days 2-4) against value 3: 2 short. An unassigned
    # shift on day 6 sets the horizon, so neither run touches its end.
    horizon = [(6, 9, 8, "L", "", -1)]
    assert raw(roster(workers(1), days(0, [0, 2, 3, 4]) + horizon), MinConsecutiveDaysCost(value=3)) == [2.0]
    assert raw(roster(workers(1), days(0, [0, 1, 2]) + horizon), MinConsecutiveDaysCost(value=3)) == [0.0]


def test_max_consecutive_days_off_cost():
    # Days 1-3 off between work on days 0 and 4: 3 against value 2 is 1 too many.
    assert raw(roster(workers(1), days(0, [0, 4])), MaxConsecutiveDaysOffCost(value=2)) == [1.0]
    assert raw(roster(workers(1), days(0, [0, 3])), MaxConsecutiveDaysOffCost(value=2)) == [0.0]


def test_rest_after_consecutive_work_cost():
    # Two worked days (work_days=2), one day off, work again: 3 - 1 rest days missing.
    spec = RestAfterConsecutiveWorkCost(work_days=2, rest_days=3)
    assert raw(roster(workers(1), days(0, [0, 1, 3])), spec) == [2.0]
    assert raw(roster(workers(1), days(0, [0, 1, 5])), spec) == [0.0]


def test_max_weekly_minutes_cost():
    # 2 x 480 minutes in week 0 against 600: 360 over; the default 2400 is not reached.
    assert raw(roster(workers(1), days(0, [0, 1])), MaxWeeklyMinutesCost(minutes=600)) == [360.0]
    assert raw(roster(workers(1), days(0, [0, 1])), MaxWeeklyMinutesCost()) == [0.0]


def test_min_weekly_minutes_cost():
    # One week: E0 works 480 of 1000 (520 short), E1 nothing (the full 1000).
    shifts = days(0, [0])
    assert raw(roster(workers(2), shifts), MinWeeklyMinutesCost(minutes=1000)) == [520.0, 1000.0]
    assert raw(roster(workers(1), shifts), MinWeeklyMinutesCost(minutes=480)) == [0.0]


def test_min_weekly_rest_days_cost():
    # Seven worked days in week 0 against a cap of 7 - 1: one too many.
    assert raw(roster(workers(1), days(0, range(7))), MinWeeklyRestDaysCost()) == [1.0]
    assert raw(roster(workers(1), days(0, range(6))), MinWeeklyRestDaysCost()) == [0.0]


def test_assignment_cost():
    # S0: 8 hours at 10 = 80; S1 overridden to 5 for E0.
    shifts = days(0, [0, 1])
    spec = AssignmentCost(cost_per_hour=10.0, shift_costs={"E0": {"S1": 5.0}})
    assert raw(roster(workers(1), shifts), spec) == [85.0]
    assert raw(roster(workers(1), shifts), AssignmentCost()) == [0.0]


def test_unassigned_shifts_cost():
    assert raw(roster(workers(1), days(0, [0]) + days(-1, [1])), UnassignedShiftsCost()) == [1.0]
    assert raw(roster(workers(1), days(0, [0, 1])), UnassignedShiftsCost()) == [0.0]


def test_shift_count_gap_cost():
    # Counts [3, 1]: gap 2; within a tolerance of 2.
    shifts = days(0, [0, 1, 2]) + days(1, [3])
    assert raw(roster(workers(2), shifts), ShiftCountGapCost()) == [2.0]
    assert raw(roster(workers(2), shifts), ShiftCountGapCost(tolerance=2)) == [0.0]


def test_weekly_minutes_gap_cost():
    # Week 0: 480 against 0 minutes, 120 tolerated: (480 - 120) / 60 = 6 hours.
    shifts = days(0, [0])
    assert raw(roster(workers(2), shifts), WeeklyMinutesGapCost()) == [6.0]
    assert raw(roster(workers(2), shifts), WeeklyMinutesGapCost(tolerance_minutes=480)) == [0.0]


def test_shift_coverage_cost():
    # Skill A holders per (day, location), wanted exactly 1: day 0 has 2 (1 over),
    # day 1 has none (its shift went to E2, who lacks A: 1 short).
    staff = [es.Employee("E0", {"A"}), es.Employee("E1", {"A"}), es.Employee("E2")]
    shifts = [(0, 9, 8, "L", "", 0), (0, 9, 8, "L", "", 1), (1, 9, 8, "L", "", 2)]
    spec = ShiftCoverageCost(worker_skill="A", min_count=1, max_count=1)
    assert raw(roster(staff, shifts), spec) == [2.0]
    assert raw(roster(staff, shifts), ShiftCoverageCost()) == [0.0]  # 0..inf never binds


def test_location_run_length_cost():
    # At L: day 0 alone (1 short of 2), days 2-5 (1 over 3). Day 7 sets the horizon.
    horizon = [(7, 9, 8, "L", "", -1)]
    spec = LocationRunLengthCost(location="L", min_run=2, max_run=3)
    assert raw(roster(workers(1), days(0, [0, 2, 3, 4, 5]) + horizon), spec) == [2.0]
    assert raw(roster(workers(1), days(0, [0, 1, 3, 4, 5]) + horizon), spec) == [0.0]


def test_rest_after_location_run_cost():
    # A day at L, then work at M the next day: 1 of the 2 rest days is worked.
    spec = RestAfterLocationRunCost(location="L", rest_days=2)
    assert raw(roster(workers(1), days(0, [0], "L") + days(0, [1], "M")), spec) == [1.0]
    assert raw(roster(workers(1), days(0, [0], "L") + days(0, [3], "M")), spec) == [0.0]


def test_location_run_gap_cost():
    # Runs at L on day 0 and day 2: one day apart against 3, 2 missing.
    spec = LocationRunGapCost(location="L", min_gap=3)
    assert raw(roster(workers(1), days(0, [0, 2])), spec) == [2.0]
    assert raw(roster(workers(1), days(0, [0, 4])), spec) == [0.0]


def test_worker_location_count_cost():
    # Shifts at L in [2, 3]: E0 has 1 (1 short), E1 has 4 (1 over).
    spec = WorkerLocationCountCost(location="L", min_count=2, max_count=3)
    assert raw(roster(workers(2), days(0, [0]) + days(1, [1, 2, 3, 4])), spec) == [1.0, 1.0]
    assert raw(roster(workers(2), days(0, [0, 1]) + days(1, [2, 3, 4])), spec) == [0.0, 0.0]


def test_day_pattern_cost():
    # L on day 0, off on day 1, M on day 2: one window of "L,OFF,M".
    spec = DayPatternCost(pattern="L,OFF,M")
    assert raw(roster(workers(1), days(0, [0], "L") + days(0, [2], "M")), spec) == [1.0]
    assert raw(roster(workers(1), days(0, [0], "L") + days(0, [1, 2], "M")), spec) == [0.0]


def test_target_days_off_cost():
    # Target days 5 and 6, both wanted off: working day 5 leaves 1 off, 1 short.
    horizon = [(6, 9, 8, "L", "", -1)]
    spec = TargetDaysOffCost(target_days=(5, 6), min_count=2)
    assert raw(roster(workers(1), days(0, [5]) + horizon), spec) == [1.0]
    assert raw(roster(workers(1), days(0, [4]) + horizon), spec) == [0.0]


def test_worker_day_count_in_range_cost():
    # Worked days in [0, 6] bounded to [2, 4]: E0 works 1 (1 short), E1 works 5 (1 over).
    spec = WorkerDayCountInRangeCost(lo=0, hi=6, min_count=2, max_count=4)
    assert raw(roster(workers(2), days(0, [0]) + days(1, [0, 1, 2, 3, 4])), spec) == [1.0, 1.0]
    assert raw(roster(workers(2), days(0, [0, 1]) + days(1, [2, 3, 4])), spec) == [0.0, 0.0]


def test_location_imbalance_cost():
    # Shifts at L: E0 2, E1 0. gap: 2 - 0; ssd: sqrt(4 - 4/2) = 1.41421.
    uneven = roster(workers(2), days(0, [0, 1]))
    assert raw(uneven, LocationImbalanceCost(location="L", metric="gap")) == [2.0]
    assert raw(uneven, LocationImbalanceCost(location="L")) == [pytest.approx(round(math.sqrt(2), 5), abs=1e-12)]
    even = roster(workers(2), days(0, [0]) + days(1, [1]))
    assert raw(even, LocationImbalanceCost(location="L")) == [0.0]


def test_worker_pairing_cost():
    buckets = {"bucket_days": (0, 1), "bucket_locations": ("L", "L")}
    # Day 0: only E0 works L; day 1: both do.
    split = roster(workers(2), days(0, [0, 1]) + days(1, [1]))
    assert raw(split, WorkerPairingCost(anchor=0, paired=1, **buckets)) == [1.0]  # should share: day 0 fails
    assert raw(split, WorkerPairingCost(anchor=0, paired=1, penalty=True, **buckets)) == [1.0]  # must not: day 1
    together = roster(workers(2), days(0, [0, 1]) + days(1, [0, 1]))
    assert raw(together, WorkerPairingCost(anchor=0, paired=1, **buckets)) == [0.0]

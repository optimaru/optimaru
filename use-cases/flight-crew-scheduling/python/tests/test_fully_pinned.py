"""A fully pinned schedule comes back unchanged, and most-pinned keeps its pins."""

import flight_crew as fc

HOUR = 60


def _schedule(pin):
    """Every seat on the one employee without the skill, so any move would pay."""
    airports = [fc.Airport("ICN", "Incheon", 37.46, 126.44), fc.Airport("NRT", "Narita", 35.77, 140.39)]
    employees = [
        fc.Employee("P1", "Pilot 1", home_airport=0, skills=["Pilot"]),
        fc.Employee("P2", "Pilot 2", home_airport=0, skills=["Pilot"]),
        fc.Employee("X1", "No skill", home_airport=0, skills=[]),
    ]
    flights = [
        fc.Flight("KE001", 0, 9 * HOUR, 1, 11 * HOUR),
        fc.Flight("KE002", 1, 13 * HOUR, 0, 15 * HOUR),
        fc.Flight("KE003", 0, 33 * HOUR, 1, 35 * HOUR),
        fc.Flight("KE004", 1, 37 * HOUR, 0, 39 * HOUR),
    ]
    assignments = [
        fc.FlightAssignment(f"a{i}", i, required_skill="Pilot", employee=2, pinned=pin(i)) for i in range(4)
    ]
    return fc.FlightCrewSchedule(airports, employees, flights, assignments)


def test_a_fully_pinned_schedule_is_not_moved():
    schedule = _schedule(lambda index: True)
    fc.solve(schedule, time_limit_seconds=0.2, random_seed=2)
    assert [assignment.employee for assignment in schedule.flight_assignments] == [2, 2, 2, 2]


def test_every_pin_holds_with_one_free_seat():
    schedule = _schedule(lambda index: index != 0)
    fc.solve(schedule, time_limit_seconds=0.2, random_seed=2)
    assert [assignment.employee for assignment in schedule.flight_assignments][1:] == [2, 2, 2]
    assert schedule.flight_assignments[0].employee != 2

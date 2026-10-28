"""제약 스팟체크: calculate_score 가 Java ConstraintProvider 의미론과 일치."""

import flight_crew as fc


def _base(airport_count: int = 2, employee_count: int = 1) -> fc.FlightCrewSchedule:
    airports = [fc.Airport(f"A{i}") for i in range(airport_count)]
    employees = [fc.Employee(str(e), f"E{e}", 0, ["Pilot"], set()) for e in range(employee_count)]
    return fc.FlightCrewSchedule(airports, employees, [], [])


def _with(schedule: fc.FlightCrewSchedule, flights, assignments) -> fc.FlightCrewSchedule:
    """flights: (dep_airport, dep_min, arr_airport, arr_min); assignments:
    (flight_index, employee_index, required_skill)."""
    return fc.FlightCrewSchedule(
        schedule.airports,
        schedule.employees,
        [fc.Flight(f"F{i}", d, dt, a, at) for i, (d, dt, a, at) in enumerate(flights)],
        [fc.FlightAssignment(str(i), f, i + 1, skill, e) for i, (f, e, skill) in enumerate(assignments)],
    )


def test_required_skill():
    # Java requiredSkill 테스트: 스킬 불일치 -100 (+ 마지막 배정 홈 미도착 -1000).
    s = _base()
    s = _with(s, [(0, 0, 1, 60)], [(0, 0, "1")])
    score = fc.calculate_score(s)
    assert score.hard == -100
    assert score.soft == -1000
    assert not score.is_feasible()


def test_flight_conflict():
    # Java flightConflict 테스트: [0,10) x [1,11) 겹침 -> -10 (홈 왕복이라 soft 0).
    s = _base()
    s = _with(s, [(0, 0, 0, 10), (0, 1, 0, 11)], [(0, 0, "Pilot"), (1, 0, "Pilot")])
    score = fc.calculate_score(s)
    assert (score.hard, score.soft) == (-10, 0)
    # 맞닿기만 하면([0,10) 뒤 [10,20)) 겹침이 아니다.
    s = _base()
    s = _with(s, [(0, 0, 0, 10), (0, 10, 0, 20)], [(0, 0, "Pilot"), (1, 0, "Pilot")])
    assert fc.calculate_score(s).hard == 0


def test_transfer_between_two_flights():
    # Java transferBetweenTwoFlights 테스트: 출발 0, 11, 13 의 0->1 세 편
    # -> 연속 쌍 (1,2), (2,3) 둘 다 공항이 안 이어져 -2
    # (+ 마지막 배정 홈 미도착 -1000).
    s = _base()
    s = _with(
        s,
        [(0, 0, 1, 10), (0, 11, 1, 12), (0, 13, 1, 14)],
        [(0, 0, "Pilot"), (1, 0, "Pilot"), (2, 0, "Pilot")],
    )
    score = fc.calculate_score(s)
    assert (score.hard, score.soft) == (-2, -1000)
    # 이어지는 체인 0->1, 1->0 은 무벌점.
    s = _base()
    s = _with(s, [(0, 0, 1, 10), (1, 20, 0, 30)], [(0, 0, "Pilot"), (1, 0, "Pilot")])
    score = fc.calculate_score(s)
    assert (score.hard, score.soft) == (0, 0)


def test_employee_unavailability():
    # Java employeeUnavailability 테스트: 출발일/비행 중 불가 -10, 하루 뒤는 0.
    day = 5
    for dep_day, expected in ((5, -10), (4, -10), (6, 0)):
        airports = [fc.Airport("A0")]
        employees = [fc.Employee("1", "E", 0, ["Pilot"], {day})]
        arr = dep_day * 1440 + (1440 + 10 if dep_day == 4 else 10)
        flights = [fc.Flight("F", 0, dep_day * 1440, 0, arr)]
        assignments = [fc.FlightAssignment("1", 0, 1, "Pilot", 0)]
        s = fc.FlightCrewSchedule(airports, employees, flights, assignments)
        assert fc.calculate_score(s).hard == expected


def test_first_last_not_home():
    # Java lastAssignmentNotArrivingAtHome 테스트: 직원0(홈0) 1->2, 2->3
    # (첫 배정 홈 미출발 -1000, 마지막 홈 미도착 -1000); 직원1(홈1) 1->2,
    # 2->1 은 무벌점.
    airports = [fc.Airport(f"A{i}") for i in range(4)]
    employees = [
        fc.Employee("1", "E0", 0, ["Pilot"], set()),
        fc.Employee("2", "E1", 1, ["Pilot"], set()),
    ]
    flights = [
        fc.Flight("F1", 1, 0, 2, 10),
        fc.Flight("F2", 2, 11, 3, 12),
        fc.Flight("F3", 1, 0, 2, 10),
        fc.Flight("F4", 2, 11, 1, 12),
    ]
    assignments = [
        fc.FlightAssignment("1", 0, 1, "Pilot", 0),
        fc.FlightAssignment("2", 1, 1, "Pilot", 0),
        fc.FlightAssignment("3", 2, 1, "Pilot", 1),
        fc.FlightAssignment("4", 3, 1, "Pilot", 1),
    ]
    s = fc.FlightCrewSchedule(airports, employees, flights, assignments)
    score = fc.calculate_score(s)
    assert (score.hard, score.soft) == (0, -2000)


def test_unassigned_contributes_nothing():
    s = _base()
    s = _with(s, [(0, 0, 1, 60)], [(0, -1, "1")])
    score = fc.calculate_score(s)
    assert (score.hard, score.soft) == (0, 0)

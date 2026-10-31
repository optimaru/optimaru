"""A fixed assignment whose score is worked out by hand.

Hard, all on Worker 01 except the skill miss:

    Shift 5 needs Skill C, which Worker 03 lacks        1
    Shift 4 on Worker 01's unavailable day            480
    Shift 1 and Shift 2 overlap from 10:00 to 12:00   120
    Shift 1 and Shift 2 start on the same day           1
                                                    -----
                                                      602

Soft: desired minutes 480 + 120 (Worker 01, day 1) and 239 (Worker 04, day 4; a
shift ending at midnight loses its last minute of the day) are rewards, undesired
minutes 480 (Worker 02, day 2) a penalty, and the shift counts 3, 1, 1, 2 cost an
unfairness of sqrt(15 - 49 / 4) = 1.65831.
"""

import es_scheduling as es

REFERENCE_SCORE = "-602hard/357.34169soft"


def reference_schedule():
    def at(day, hour):
        return es.minutes(day, hour)

    employees = [
        es.Employee("Worker 01", {"Skill A", "Skill B"}, unavailable_dates={3}, desired_dates={1}),
        es.Employee("Worker 02", {"Skill A"}, undesired_dates={2}),
        es.Employee("Worker 03", {"Skill B"}),
        es.Employee("Worker 04", {"Skill C", "Skill A"}, desired_dates={4}),
    ]
    shifts = [
        es.Shift("Shift 1", at(1, 9), at(1, 17), "Location A", "Skill A"),
        es.Shift("Shift 2", at(1, 10), at(1, 12), "Location A", "Skill B"),
        es.Shift("Shift 3", at(2, 9), at(2, 17), "Location A", "Skill A"),
        es.Shift("Shift 4", at(3, 9), at(3, 17), "Location A", "Skill B"),
        es.Shift("Shift 5", at(4, 9), at(4, 17), "Location A", "Skill C"),
        es.Shift("Shift 6", at(4, 20), at(5, 0), "Location A", "Skill A"),
        es.Shift("Shift 7", at(1, 9), at(1, 17), "Location A", "Skill A"),
    ]
    for shift, employee in zip(shifts, [0, 0, 1, 0, 2, 3, 3], strict=True):
        shift.employee_index = employee
    return es.EmployeeSchedule(employees, shifts)

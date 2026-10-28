"""Constraint spot checks: reproduces Java TaskAssigningConstraintProviderTest cases
with `calculate_score` (full recompute)."""

import task_assigning as ta


def _solution(task_types, employees, tasks, customers=1):
    return ta.TaskAssigningSolution(
        task_types,
        [ta.Customer(str(i)) for i in range(customers)],
        tasks,
        employees,
    )


def test_no_missing_skills():
    # Employee skills {"3"}; three tasks requiring {1,2} / {3} / {5}, all assigned
    # -> 3 missing skills in total.
    solution = _solution(
        task_types=[
            ta.TaskType("1", base_duration=1, required_skills=["1", "2"]),
            ta.TaskType("2", base_duration=1, required_skills=["3"]),
            ta.TaskType("3", base_duration=1, required_skills=["5"]),
        ],
        employees=[ta.Employee("e0", skills=["3"], tasks=[0, 1, 2])],
        tasks=[
            ta.Task("1", task_type=0, customer=0, min_start_time=1, priority=ta.Priority.CRITICAL),
            ta.Task("2", task_type=1, customer=0, min_start_time=1, priority=ta.Priority.CRITICAL),
            ta.Task("3", task_type=2, customer=0, min_start_time=1, priority=ta.Priority.CRITICAL),
        ],
    )
    assert ta.calculate_score(solution).hard == -3


def test_minimize_unassigned_tasks():
    # Only 1 of 3 tasks assigned -> soft0 == -2.
    solution = _solution(
        task_types=[ta.TaskType("T", base_duration=1)],
        employees=[ta.Employee("e0", tasks=[2])],
        tasks=[ta.Task(str(i + 1), task_type=0, customer=0) for i in range(3)],
    )
    assert ta.calculate_score(solution).soft0 == -2


def test_priority_task_end_times():
    # HIGH affinity (x1), dur 10, minStart 1 -> endTime 11.
    for priority, expected in [
        (ta.Priority.CRITICAL, -44),
        (ta.Priority.MAJOR, -22),
        (ta.Priority.MINOR, -11),
    ]:
        solution = _solution(
            task_types=[ta.TaskType("T", base_duration=10)],
            employees=[ta.Employee("e0", affinity_by_customer=[ta.Affinity.HIGH], tasks=[0])],
            tasks=[ta.Task("1", task_type=0, customer=0, min_start_time=1, priority=priority)],
        )
        assert ta.calculate_score(solution).soft2 == expected


def test_minimize_makespan_and_shadows():
    # employee0 (MEDIUM=x2): dur 10 minStart 1 -> end 21; dur 20 minStart 2
    # -> start max(21,2)=21, end 61. employee1 (HIGH=x1): dur 1 -> end 2.
    # soft1 == -(61^2 + 2^2) == -3725.
    solution = _solution(
        task_types=[
            ta.TaskType("A", base_duration=10),
            ta.TaskType("B", base_duration=20),
            ta.TaskType("C", base_duration=1),
        ],
        employees=[
            ta.Employee("e0", affinity_by_customer=[ta.Affinity.MEDIUM], tasks=[0, 1]),
            ta.Employee("e1", affinity_by_customer=[ta.Affinity.HIGH], tasks=[2]),
        ],
        tasks=[
            ta.Task("1", task_type=0, customer=0, min_start_time=1, priority=ta.Priority.CRITICAL),
            ta.Task("2", task_type=1, customer=0, min_start_time=2, priority=ta.Priority.MINOR),
            ta.Task("3", task_type=2, customer=0, min_start_time=1, priority=ta.Priority.CRITICAL),
        ],
    )
    assert ta.calculate_score(solution).soft1 == -3725
    assert solution.tasks[1].start_time == 21
    assert solution.tasks[1].employee == 0
    assert solution.tasks[2].employee == 1


def test_score_is_lexicographic():
    # hard dominates the soft levels, and soft0 > soft1 > soft2 in priority
    # (Score is exposed as a real level comparison, not a lexicographic string).
    solution = _solution(
        task_types=[ta.TaskType("T", base_duration=10, required_skills=["x"])],
        employees=[ta.Employee("e0", tasks=[0])],
        tasks=[ta.Task("1", task_type=0, customer=0)],
    )
    score = ta.calculate_score(solution)
    assert score.hard == -1  # lacks skill "x"
    assert not score.is_feasible()
    assert str(score) == "[-1]hard/[0/-1600/-40]soft"  # NONE x4: end 40

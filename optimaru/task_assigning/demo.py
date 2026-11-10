"""Reproduction of the Java quickstart DemoDataGenerator: returns a 4 task type, 4 customer,
8 employee, 28 task instance as a pydantic model."""

from .models import CustomerModel, EmployeeModel, TaskAssigningSolutionModel, TaskModel, TaskTypeModel

__all__ = ["generate_demo_data"]

PROBLEM_SOLVING = "Problem Solving"
TEAM_BUILDING = "Team Building"
BUSINESS_STORYTELLING = "Business Storytelling"
RISK_MANAGEMENT = "Risk Management"
CREATIVE_THINKING = "Creative Thinking"
STRATEGIC_PLANNING = "Strategic Planning"

# (code, title, base_duration, required_skills)
_TASK_TYPES = [
    ("IS", "Improve Sales", 46, [STRATEGIC_PLANNING]),
    ("ET", "Expand Tax", 63, [PROBLEM_SOLVING, TEAM_BUILDING]),
    ("SV", "Shrink VAT", 63, [STRATEGIC_PLANNING]),
    ("AL", "Approve Legal", 40, [RISK_MANAGEMENT]),
]

# (id, name)
_CUSTOMERS = [
    ("1", "Steel Inc"),
    ("2", "Paper Corp"),
    ("3", "Stone Limited"),
    ("4", "Wood Express"),
]

# (full_name, skills, affinity per customer id)
_EMPLOYEES = [
    (
        "Amy",
        [PROBLEM_SOLVING, BUSINESS_STORYTELLING, TEAM_BUILDING],
        {"1": "HIGH", "2": "MEDIUM", "3": "HIGH", "4": "MEDIUM"},
    ),
    ("Beth", [RISK_MANAGEMENT, CREATIVE_THINKING], {"1": "LOW", "2": "HIGH", "3": "LOW", "4": "MEDIUM"}),
    ("Carl", [STRATEGIC_PLANNING, PROBLEM_SOLVING], {"1": "MEDIUM", "2": "HIGH", "3": "MEDIUM", "4": "LOW"}),
    ("Dan", [BUSINESS_STORYTELLING, TEAM_BUILDING], {"1": "LOW", "2": "HIGH", "3": "HIGH", "4": "MEDIUM"}),
    (
        "Elsa",
        [RISK_MANAGEMENT, STRATEGIC_PLANNING, CREATIVE_THINKING],
        {"1": "MEDIUM", "2": "HIGH", "3": "HIGH", "4": "MEDIUM"},
    ),
    ("Flo", [PROBLEM_SOLVING, TEAM_BUILDING], {"1": "HIGH", "2": "HIGH", "3": "NONE", "4": "MEDIUM"}),
    (
        "Gus",
        [RISK_MANAGEMENT, CREATIVE_THINKING, BUSINESS_STORYTELLING],
        {"1": "LOW", "2": "MEDIUM", "3": "MEDIUM", "4": "LOW"},
    ),
    (
        "Hugo",
        [STRATEGIC_PLANNING, PROBLEM_SOLVING, TEAM_BUILDING],
        {"1": "NONE", "2": "MEDIUM", "3": "NONE", "4": "LOW"},
    ),
]

# (task_type_code, index_in_task_type, customer_id, priority) — Java
# The 28 tasks of DemoDataGenerator.generateTasks, in the same order (ids 1..28).
_TASKS = [
    ("SV", 1, "2", "MINOR"),
    ("AL", 1, "4", "MAJOR"),
    ("SV", 2, "4", "MAJOR"),
    ("IS", 1, "4", "CRITICAL"),
    ("AL", 2, "3", "MINOR"),
    ("AL", 3, "3", "MINOR"),
    ("ET", 1, "2", "MINOR"),
    ("ET", 2, "1", "MINOR"),
    ("ET", 3, "1", "MINOR"),
    ("SV", 3, "4", "MAJOR"),
    ("IS", 2, "4", "MINOR"),
    ("ET", 4, "1", "MAJOR"),
    ("IS", 3, "3", "MINOR"),
    ("ET", 5, "4", "MAJOR"),
    ("ET", 6, "3", "CRITICAL"),
    ("ET", 7, "2", "MINOR"),
    ("AL", 4, "3", "MAJOR"),
    ("AL", 5, "1", "MAJOR"),
    ("IS", 4, "4", "MAJOR"),
    ("IS", 5, "4", "CRITICAL"),
    ("IS", 6, "1", "MINOR"),
    ("IS", 7, "2", "MAJOR"),
    ("IS", 8, "4", "CRITICAL"),
    ("AL", 6, "4", "MINOR"),
    ("AL", 7, "1", "CRITICAL"),
    ("IS", 9, "4", "MAJOR"),
    ("IS", 10, "1", "CRITICAL"),
    ("IS", 11, "4", "MAJOR"),
]


def generate_demo_data() -> TaskAssigningSolutionModel:
    """The same demo instance as Java DemoDataGenerator (every task unassigned)."""
    return TaskAssigningSolutionModel(
        task_types=[
            TaskTypeModel(code=code, title=title, base_duration=duration, required_skills=skills)
            for code, title, duration, skills in _TASK_TYPES
        ],
        customers=[CustomerModel(id=id_, name=name) for id_, name in _CUSTOMERS],
        tasks=[
            TaskModel(
                id=str(i + 1),
                task_type_code=code,
                index_in_task_type=index,
                customer_id=customer_id,
                priority=priority,
            )
            for i, (code, index, customer_id, priority) in enumerate(_TASKS)
        ],
        employees=[
            EmployeeModel(
                id=str(i + 1),
                full_name=name,
                skills=skills,
                customer_to_affinity=affinities,
            )
            for i, (name, skills, affinities) in enumerate(_EMPLOYEES)
        ],
    )

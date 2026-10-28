"""pydantic mirror serialization/validation tests."""

import pytest
from pydantic import ValidationError

from task_assigning.demo import generate_demo_data
from task_assigning.models import (
    CustomerModel,
    EmployeeModel,
    TaskAssigningSolutionModel,
    TaskModel,
    TaskTypeModel,
)


def test_json_round_trip():
    model = generate_demo_data()
    payload = model.model_dump_json()
    restored = TaskAssigningSolutionModel.model_validate_json(payload)
    assert restored == model


def test_to_core_from_core_round_trip():
    model = generate_demo_data()
    core = model.to_core()
    assert len(core.tasks) == 28
    assert len(core.employees) == 8
    restored = TaskAssigningSolutionModel.from_core(core)
    # Scores are not computed yet (the initial 0 is carried as-is); compare structure only.
    assert [t.id for t in restored.tasks] == [t.id for t in model.tasks]
    assert [e.full_name for e in restored.employees] == [e.full_name for e in model.employees]
    assert restored.employees[0].customer_to_affinity == model.employees[0].customer_to_affinity


def _tiny_kwargs(**overrides):
    kwargs = {
        "task_types": [TaskTypeModel(code="T", base_duration=10)],
        "customers": [CustomerModel(id="c1")],
        "tasks": [TaskModel(id="t1", task_type_code="T", customer_id="c1")],
        "employees": [EmployeeModel(id="e1")],
    }
    kwargs.update(overrides)
    return kwargs


def test_unknown_task_type_rejected():
    with pytest.raises(ValidationError, match="task_type_code"):
        TaskAssigningSolutionModel(**_tiny_kwargs(tasks=[TaskModel(id="t1", task_type_code="X", customer_id="c1")]))


def test_unknown_customer_rejected():
    with pytest.raises(ValidationError, match="customer_id"):
        TaskAssigningSolutionModel(**_tiny_kwargs(tasks=[TaskModel(id="t1", task_type_code="T", customer_id="cX")]))


def test_duplicate_assignment_rejected():
    with pytest.raises(ValidationError, match="more than one employee"):
        TaskAssigningSolutionModel(
            **_tiny_kwargs(
                employees=[
                    EmployeeModel(id="e1", tasks=["t1"]),
                    EmployeeModel(id="e2", tasks=["t1"]),
                ]
            )
        )


def test_invalid_priority_rejected():
    with pytest.raises(ValidationError, match="priority"):
        TaskModel(id="t1", task_type_code="T", customer_id="c1", priority="URGENT")


def test_invalid_affinity_rejected():
    with pytest.raises(ValidationError, match="affinity"):
        EmployeeModel(id="e1", customer_to_affinity={"c1": "BEST"})


def test_core_validates_task_lists():
    import task_assigning as ta

    model = generate_demo_data()
    core = model.to_core()
    with pytest.raises(ValueError):
        core.set_task_lists([[0]] * len(core.employees))  # duplicate assignment
    with pytest.raises(ValueError):
        core.set_task_lists([[0]])  # employee count mismatch
    lists = core.task_lists()
    lists[0] = [0, 1]
    core.set_task_lists(lists)
    assert core.task_lists()[0] == [0, 1]
    assert isinstance(core, ta.TaskAssigningSolution)

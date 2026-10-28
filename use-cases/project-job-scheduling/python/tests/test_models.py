"""pydantic 모델 직렬화/검증/엔진 변환 테스트."""

import pytest
from pydantic import ValidationError

import project_job as pj
from project_job.demo import generate_demo_data
from project_job.models import (
    AllocationModel,
    ExecutionModeModel,
    JobModel,
    ProjectJobScheduleModel,
    ProjectModel,
    ResourceModel,
)


def test_demo_json_roundtrip():
    model = generate_demo_data()
    payload = model.model_dump_json()
    restored = ProjectJobScheduleModel.model_validate_json(payload)
    assert restored == model


def test_to_core_from_core_roundtrip():
    model = generate_demo_data()
    core = model.to_core()
    pj.calculate_score(core)
    solved = ProjectJobScheduleModel.from_core(core)
    # 구조 필드는 그대로 살아 돌아온다 (successor 리스트 포함; 순서 무관).
    assert [p.id for p in solved.projects] == [p.id for p in model.projects]
    assert [r.id for r in solved.resources] == [r.id for r in model.resources]
    assert [j.id for j in solved.jobs] == [j.id for j in model.jobs]
    for original, restored in zip(model.jobs, solved.jobs):
        assert sorted(original.successor_job_ids) == sorted(restored.successor_job_ids)
        assert [m.id for m in original.execution_modes] == [m.id for m in restored.execution_modes]
    # SOURCE/SINK 는 고정값 (첫 mode, delay 0) 그대로.
    for job, allocation in zip(solved.jobs, solved.allocations):
        if job.job_type != "STANDARD":
            assert allocation.execution_mode_id == job.execution_modes[0].id
            assert allocation.delay == 0


def test_mode_delay_encoding():
    model = generate_demo_data()
    core = model.to_core()
    assert core.max_mode_count == 3
    assert core.value_count() == 500 * 3
    value = core.encode_value(7, 2)
    assert core.delay_of_value(value) == 7
    assert core.mode_slot_of_value(value) == 2
    # SOURCE (mode 1개) 잡: slot 2 -> mode index 2 % 1 == 0.
    assert core.execution_mode_index_of_value(0, value) == 0
    with pytest.raises(ValueError):
        core.encode_value(500, 0)  # delay 범위 밖
    with pytest.raises(ValueError):
        core.encode_value(0, 3)  # slot 범위 밖


def _minimal_kwargs():
    jobs = [
        JobModel(
            id="s",
            project_id="P",
            job_type="SOURCE",
            execution_modes=[ExecutionModeModel(id="m0", duration=0)],
            successor_job_ids=["t"],
        ),
        JobModel(
            id="t",
            project_id="P",
            execution_modes=[ExecutionModeModel(id="m1", duration=3)],
        ),
    ]
    allocations = [
        AllocationModel(id="s", job_id="s", execution_mode_id="m0", delay=0),
        AllocationModel(id="t", job_id="t"),
    ]
    return {
        "projects": [ProjectModel(id="P")],
        "resources": [ResourceModel(id="R", capacity=1, renewable=True, project_id="P")],
        "jobs": jobs,
        "allocations": allocations,
    }


def test_validation_rejects_bad_references():
    kwargs = _minimal_kwargs()
    ProjectJobScheduleModel(**kwargs)  # 기준: 유효

    bad = _minimal_kwargs()
    bad["jobs"][1].project_id = "NOPE"
    with pytest.raises(ValidationError):
        ProjectJobScheduleModel(**bad)

    bad = _minimal_kwargs()
    bad["allocations"] = bad["allocations"][:1]  # job 과 1:1 이 아님
    with pytest.raises(ValidationError):
        ProjectJobScheduleModel(**bad)

    bad = _minimal_kwargs()
    bad["allocations"][1].execution_mode_id = "m0"  # 다른 job 의 mode
    with pytest.raises(ValidationError):
        ProjectJobScheduleModel(**bad)


def test_global_resource_must_be_renewable():
    with pytest.raises(ValidationError):
        ResourceModel(id="G", capacity=5, renewable=False)


def test_cycle_rejected_by_core():
    kwargs = _minimal_kwargs()
    kwargs["jobs"][1].successor_job_ids = ["s"]  # s -> t -> s 사이클
    model = ProjectJobScheduleModel(**kwargs)
    with pytest.raises(ValueError):
        model.to_core()

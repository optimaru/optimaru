from project_job_cpsat.public_benchmark import critical_path, make_model, parse_instance

SAMPLE = """\
4 1
2
0 0 2 2 3
2 1 1 4
3 2 1 4
0 0 0
"""


def test_parse_and_convert_hexaly_rcpsp() -> None:
    instance = parse_instance(SAMPLE, name="tiny")
    assert critical_path(instance) == 3
    model = make_model(instance)
    assert model.projects[0].critical_path_duration == 3
    assert model.resources[0].capacity == 2
    assert model.jobs[0].successor_job_ids == ["J2", "J3"]
    assert model.allocations[0].pinned
    assert model.allocations[-1].pinned

import hashlib
import io
import json
import zipfile

import pytest

from project_job_scheduling_cpsat.public_benchmark import (
    INSTANCE_SHA256,
    _verified,
    critical_path,
    instance_names,
    load_instance,
    main,
    make_model,
    parse_instance,
    run_case,
    synthetic_instance,
)

SAMPLE = """\
4 1
2
0 0 2 2 3
2 1 1 4
3 2 1 4
0 0 0
"""


def test_parse_and_convert_patterson_rcpsp() -> None:
    instance = parse_instance(SAMPLE, name="tiny")
    assert critical_path(instance) == 3
    model = make_model(instance)
    assert model.projects[0].critical_path_duration == 3
    assert model.resources[0].capacity == 2
    assert model.jobs[0].successor_job_ids == ["J2", "J3"]
    assert model.allocations[0].pinned
    assert model.allocations[-1].pinned


def _archive(files: dict[str, bytes]) -> bytes:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for name, payload in files.items():
            archive.writestr(name, payload)
    return buffer.getvalue()


def test_archive_is_checked_by_its_instance_files(monkeypatch) -> None:
    files = {f"rcpsp/instances/Pat{i}.rcp": f"instance {i}".encode() for i in range(1, 6)}
    pins = {name.rsplit("/", 1)[1][:-4]: hashlib.sha256(payload).hexdigest() for name, payload in files.items()}
    monkeypatch.setattr("project_job_scheduling_cpsat.public_benchmark.INSTANCE_SHA256", pins)
    # Other members (the vendor's sample programs, a further instance) and the zip's own
    # bytes do not matter.
    assert _verified(_archive(files | {"rcpsp/rcpsp.py": b"anything"}))
    assert _verified(_archive(files | {"rcpsp/instances/Pat6.rcp": b"one more"}))
    assert not _verified(_archive(files | {"rcpsp/instances/Pat1.rcp": b"edited"}))
    assert not _verified(_archive({name: files[name] for name in list(files)[:4]}))
    assert not _verified(b"<html>not a zip</html>")
    assert instance_names() == sorted(INSTANCE_SHA256) == ["Pat1", "Pat2", "Pat3", "Pat4", "Pat5"]


def test_run_case_reports_both_backends() -> None:
    lines: list[str] = []
    result = run_case(
        parse_instance(SAMPLE, name="tiny"),
        seconds=1,
        seed=0,
        workers=1,
        backends=("engine", "cpsat"),
        log=lines.append,
    )
    assert (result["tasks"], result["resources"], result["critical_path"]) == (4, 1, 3)
    assert (result["budget"], result["seed"], result["workers"]) == (1, 0, 1)
    assert [run["solver"] for run in result["runs"]] == ["engine", "cp-sat"]
    for run in result["runs"]:
        # Tasks 2 and 3 need 1 and 2 of the 2 units: they cannot overlap, so 2 + 3.
        assert run["feasible"] and run["makespan"] == 5 and run["status"] == ""
    assert lines[0].startswith("tiny: 4 tasks, 1 resources")


def test_one_choice_of_instances_per_run() -> None:
    for argv in (
        ["--all", "--instance", "Pat1"],
        ["--all", "--synthetic", "5"],
        ["--instance", "Pat1", "--synthetic", "5"],
        ["--instance", "Pat1", "--synthetic-seeds", "0,1"],
    ):
        with pytest.raises(SystemExit) as exit_info:
            main(argv)
        assert exit_info.value.code == 2


def test_a_generated_instance_is_seeded_and_has_the_shape_of_the_set() -> None:
    instance = synthetic_instance(30, seed=3)
    assert instance == synthetic_instance(30, seed=3)
    assert instance != synthetic_instance(30, seed=4)
    assert instance.name == "synthetic-30-s3"
    assert len(instance.tasks) == 32 and len(instance.capacities) == 4

    source, *activities, sink = instance.tasks
    assert source.duration == sink.duration == 0 and sink.successors == ()
    assert source.requirements == sink.requirements == (0, 0, 0, 0)
    for index, task in enumerate(activities, start=1):
        assert 1 <= task.duration <= 10
        # An activity fits the capacities on its own, and uses one or two of the resources.
        assert all(need <= capacity for need, capacity in zip(task.requirements, instance.capacities))
        assert 1 <= sum(need > 0 for need in task.requirements) <= 2
        # Successors come later, so the precedence graph has no cycle; the last ones precede the sink.
        assert task.successors and all(index < successor <= 31 for successor in task.successors)
    # Every activity is reached from the source.
    reached, frontier = {0}, [0]
    while frontier:
        for successor in instance.tasks[frontier.pop()].successors:
            if successor not in reached:
                reached.add(successor)
                frontier.append(successor)
    assert reached == set(range(32))
    assert critical_path(instance) >= max(task.duration for task in activities)
    assert len(make_model(instance).jobs) == 32

    with pytest.raises(ValueError, match="at least one activity"):
        synthetic_instance(0)


def test_json_output_lists_one_result_per_generated_size_and_seed(capsys) -> None:
    argv = ["--synthetic", "4", "--synthetic", "6", "--synthetic-seeds", "0,5"]
    assert main([*argv, "--seconds", "1", "--backend", "engine", "--json"]) == 0
    results = json.loads(capsys.readouterr().out)
    assert [result["instance"] for result in results] == [
        "synthetic-4-s0",
        "synthetic-4-s5",
        "synthetic-6-s0",
        "synthetic-6-s5",
    ]
    assert [result["tasks"] for result in results] == [6, 6, 8, 8]
    assert all(run["solver"] == "engine" and run["feasible"] for result in results for run in result["runs"])


def test_an_instance_that_is_not_pinned_is_not_loaded(tmp_path) -> None:
    # Refused by name, before the archive is read or downloaded.
    with pytest.raises(ValueError, match="unknown instance 'Pat6'; available: Pat1, Pat2, Pat3, Pat4, Pat5"):
        load_instance("Pat6", tmp_path)

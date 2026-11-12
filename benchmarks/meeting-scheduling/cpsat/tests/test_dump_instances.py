"""`benchmarks/meeting-scheduling/dump_instances.py` writes what its docstring says.

The C++ reader is checked against the writer by the script's `--check` (CI runs it with the
built bench); here the writer alone, without a binary.
"""

import importlib.util
import shlex
from pathlib import Path

from meeting_scheduling_cpsat import benchmark
from optimaru.meeting_scheduling.models import MeetingScheduleModel

SCRIPT = Path(__file__).resolve().parents[2] / "dump_instances.py"


def _load():
    spec = importlib.util.spec_from_file_location("dump_instances", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_the_demo_data_is_written_in_the_documented_order():
    dump = _load()
    problem = benchmark.instances()["demo"]()
    model = MeetingScheduleModel.from_core(problem)
    lines = dump.to_text(model).splitlines()
    assert lines[0] == "MS1"
    sections = {line.split()[0]: index for index, line in enumerate(lines) if line.split()[0] in dump.SECTIONS}
    assert list(sections) == list(dump.SECTIONS)
    tables = (model.people, model.time_grains, model.rooms, model.meetings, model.meeting_assignments)
    for name, rows in zip(dump.SECTIONS, tables):
        assert lines[sections[name]] == f"{name} {len(rows)}"
    assert len(lines) == 1 + 5 + sum(len(rows) for rows in tables)
    # A meeting is its id, topic, duration, two counted lists and a flag.
    for meeting, line in zip(model.meetings, lines[sections["meetings"] + 1 :]):
        tokens = shlex.split(line)
        assert len(tokens) == 6 + len(meeting.required_attendees) + len(meeting.preferred_attendees)
        assert tokens[:3] == [meeting.id, meeting.topic, str(meeting.duration_in_grains)]
    for assignment, line in zip(model.meeting_assignments, lines[sections["meetingAssignments"] + 1 :]):
        assert shlex.split(line) == [assignment.id, str(assignment.meeting), "-1", "0"]


def test_the_fixed_assignment_scores_the_same_twice_and_leaves_the_problem_alone():
    dump = _load()
    problem = benchmark.instances()["demo"]()
    before = MeetingScheduleModel.from_core(problem).model_dump()
    score, costs = dump.crosscheck(problem)
    assert (score, costs) == dump.crosscheck(problem)
    assert score == "hard=-258 soft=-340007209"
    assert costs and all(name[0].islower() and "_" not in name and cost > 0 for name, cost in costs.items())
    assert MeetingScheduleModel.from_core(problem).model_dump() == before

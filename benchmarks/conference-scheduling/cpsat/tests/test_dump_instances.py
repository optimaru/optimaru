"""`benchmarks/conference-scheduling/dump_instances.py` writes what its docstring says.

The C++ reader is checked against the writer by the script's `--check` (CI runs it with the
built bench); here the writer alone, without a binary.
"""

import importlib.util
import shlex
from pathlib import Path

from conference_scheduling_cpsat import benchmark
from optimaru.conference_scheduling.models import ConferenceScheduleModel

SCRIPT = Path(__file__).resolve().parents[2] / "dump_instances.py"


def _load():
    spec = importlib.util.spec_from_file_location("dump_instances", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_the_demo_conference_is_written_in_the_documented_order():
    dump = _load()
    problem = benchmark.instances()["demo"]()
    model = ConferenceScheduleModel.from_core(problem)
    lines = dump.to_text(model).splitlines()
    assert lines[:4] == [
        "CS1",
        'name "Conference"',
        "minimumConsecutiveTalksPauseInMinutes 30",
        'talkTypes 2 "Lab" "Breakout"',
    ]
    sections = {line.split()[0]: index for index, line in enumerate(lines) if line.split()[0] in dump.SECTIONS}
    assert list(sections) == list(dump.SECTIONS)
    for name, rows in zip(dump.SECTIONS, (model.timeslots, model.rooms, model.speakers, model.talks)):
        assert lines[sections[name]] == f"{name} {len(rows)}"
    # A quoted string with a space is one token; a talk is 9 values and 15 counted lists.
    room = shlex.split(lines[sections["rooms"] + 5])
    assert room == ["R5", "Room E (LAB)", "490", "1", "0", "0", "1", "Recorded"]
    for talk, line in zip(model.talks, lines[sections["talks"] + 1 :]):
        tokens = shlex.split(line)
        lists = sum(len(getattr(talk, name)) for name in dump.TALK_LISTS)
        assert len(tokens) == 9 + 15 + lists, talk.code
        assert tokens[0] == talk.code and tokens[-2:] == ["-1", "0"]
    assert len(lines) == 1 + 3 + 4 + len(model.timeslots) + len(model.rooms) + len(model.speakers) + len(model.talks)


def test_a_quote_or_a_backslash_in_a_string_is_escaped():
    dump = _load()
    assert dump._string('a "b" \\ c') == '"a \\"b\\" \\\\ c"'


def test_the_fixed_assignment_scores_the_same_twice_and_leaves_the_problem_alone():
    dump = _load()
    problem = benchmark.instances()["demo"]()
    before = ConferenceScheduleModel.from_core(problem).model_dump()
    score, costs = dump.crosscheck(problem)
    assert (score, costs) == dump.crosscheck(problem)
    assert score == "hard=-90138300 soft=-151380"
    assert costs and all(name[0].islower() and "_" not in name and cost > 0 for name, cost in costs.items())
    assert ConferenceScheduleModel.from_core(problem).model_dump() == before

"""Serialization/deserialization of the pydantic mirror (models.py) and demo data reproduction checks."""

from tournament_scheduling.models import (
    TeamAssignmentModel,
    TournamentScheduleModel,
    demo_data,
)


def test_demo_data_shape():
    model = demo_data()
    assert [t.name for t in model.teams] == [
        "Maarten",
        "Geoffrey",
        "Lukas",
        "Chris",
        "Fred",
        "Radek",
        "Maciej",
    ]
    assert len(model.days) == 18
    assert len(model.unavailability_penalties) == 12
    assert len(model.team_assignments) == 72
    # (team, day) pairs must be unique (the Java generator's dedupe).
    pairs = {(p.team, p.day) for p in model.unavailability_penalties}
    assert len(pairs) == 12
    # 4 slots per day, unique ids.
    ids = {a.id for a in model.team_assignments}
    assert len(ids) == 72
    for d in range(18):
        assert sum(1 for a in model.team_assignments if a.day == d) == 4


def test_json_round_trip():
    model = demo_data()
    model.team_assignments[0].team = 3
    model.team_assignments[0].pinned = True
    clone = TournamentScheduleModel.model_validate_json(model.model_dump_json())
    assert clone == model


def test_core_round_trip():
    model = demo_data()
    model.team_assignments[5].team = 2
    core = model.to_core()
    assert core.team_count() == 7
    assert core.day_count() == 18
    assert core.assignment()[5] == 2
    back = TournamentScheduleModel.from_core(core)
    # from_core fills score 0 (not computed yet); everything else must match.
    assert back.score_hard == 0
    back2 = back.model_copy(update={"score_hard": None, "score_soft": None})
    assert back2 == model


def test_unassigned_team_maps_to_none():
    model = demo_data()
    core = model.to_core()
    assert core.assignment() == [-1] * 72
    back = TournamentScheduleModel.from_core(core)
    assert all(a.team is None for a in back.team_assignments)


def test_assignment_model_defaults():
    a = TeamAssignmentModel(id=1, day=0)
    assert a.team is None
    assert a.pinned is False
    assert a.index_in_day == 0

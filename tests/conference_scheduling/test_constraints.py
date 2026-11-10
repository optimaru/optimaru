"""Constraint spot checks: re-verifies some hand-computed cases of the C++ tests (a) through the Python bindings.

The raw costs come from `constraint_costs()` (every constraint is global: one total
each), the only per-constraint surface; the three rewards report their
total negated (raw <= 0).
"""

import optimaru.conference_scheduling as cs
from optimaru.conference_scheduling.constraints import score


def raw_totals(schedule):
    """Raw cost per constraint name (the one total of each global constraint)."""
    return {name: sum(entry["costs"]) for name, entry in cs.constraint_costs(schedule).items()}


def default_score(schedule):
    """The default score, rebuilt in pure Python from the raw costs."""
    return score(cs.constraint_costs(schedule))


def make_base() -> cs.ConferenceSchedule:
    """The same base instance as makeBase in cpp/cs_test.cpp."""

    def slot(sid, start, end):
        return cs.Timeslot(id=sid, start_minute=start, end_minute=end, talk_types=[0])

    return cs.ConferenceSchedule(
        name="test",
        talk_types=["B"],
        timeslots=[
            slot("s0", 540, 600),
            slot("s1", 540, 600),
            slot("s2", 570, 630),
            slot("s3", 630, 690),
            slot("s4", 1980, 2040),
            slot("s5", 3420, 3480),
        ],
        rooms=[
            cs.Room("r0", capacity=100, talk_types=[0]),
            cs.Room("r1", capacity=200, talk_types=[0]),
            cs.Room("r2", capacity=300, talk_types=[0]),
        ],
        speakers=[cs.Speaker(f"k{k}") for k in range(4)],
        talks=[],
    )


def place(schedule, code, speakers, slot, room, **kwargs):
    talk = cs.Talk(code=code, speakers=speakers, **kwargs)
    talk.slot_room = schedule.slot_room_of(slot, room)
    talks = schedule.talks
    talks.append(talk)
    schedule.talks = talks
    return talk


def test_room_conflict():
    c = make_base()
    place(c, "A", [0], 0, 0)
    place(c, "B", [1], 1, 0)  # same times (distinct slots), same room
    raw = raw_totals(c)
    assert raw["room_conflict_cost"] == 60
    assert default_score(c).hard == -60000
    assert cs.calculate_score(c).hard == -60000


def test_speaker_conflict_and_pause():
    c = make_base()
    place(c, "A", [0, 1], 0, 0)
    place(c, "B", [0, 1], 1, 1)
    raw = raw_totals(c)
    assert raw["speaker_conflict_cost"] == 120  # 2 shared speakers x 60 overlap
    assert raw["consecutive_talks_pause_cost"] == 120  # overlapping pair, no pause
    assert default_score(c).hard == -(10 * 120 + 120)


def test_talk_type_incompatibility_synthetic():
    c = make_base()
    c.talk_types = ["B", "L"]
    place(c, "A", [0], 0, 0, talk_type=1)  # "L" talk in "B"-only slot+room
    raw = raw_totals(c)
    assert raw["talk_type_incompatibility_cost"] == 120
    assert default_score(c).hard == -12000000


def test_prerequisite_and_popular():
    c = make_base()
    place(c, "A", [0], 3, 2, favorite_count=10)  # 630-690, capacity 300
    place(c, "B", [1], 0, 0, favorite_count=20, prerequisite_talks=[0])
    raw = raw_totals(c)
    # A (prerequisite) ends 690 > B starts 540: violated, combined 120.
    assert raw["talk_prerequisite_talks_cost"] == 120
    # B is more favorited but sits in the smaller room.
    assert raw["popular_talks_room_size_cost"] == 120


def test_diversity_rewards():
    c = make_base()
    place(c, "A", [0], 0, 0, audience_types=["Dev"], audience_level=1)
    place(c, "B", [1], 0, 1, audience_types=["Dev"], audience_level=2, language="de")
    raw = raw_totals(c)
    # Rewards: constraint_costs() reports each reward negated (raw <= 0).
    assert -raw["audience_type_diversity_cost"] == 60
    assert -raw["audience_level_diversity_cost"] == 60
    assert -raw["language_diversity_cost"] == 60
    total = default_score(c)
    assert total.hard == 0
    assert total.soft == 720  # 1*60 + 1*60 + 10*60
    assert cs.calculate_score(c).soft == total.soft


def test_speaker_makespan():
    c = make_base()
    place(c, "A", [0], 0, 0)  # day 0
    place(c, "B", [0], 5, 1)  # day 2 -> span 2 -> one 8h day
    raw = raw_totals(c)
    assert raw["speaker_makespan_cost"] == 480
    assert default_score(c).soft == -20 * 480


def test_tag_constraints():
    c = make_base()
    timeslots = c.timeslots
    timeslots[0].tags = ["TS"]
    c.timeslots = timeslots
    rooms = c.rooms
    rooms[0].tags = ["RM"]
    c.rooms = rooms
    place(
        c,
        "A",
        [0],
        0,
        0,
        required_timeslot_tags=["TS", "X"],  # missing X
        undesired_room_tags=["RM"],  # prevailing RM
    )
    raw = raw_totals(c)
    assert raw["talk_required_timeslot_tags_cost"] == 60
    assert raw["talk_undesired_room_tags_cost"] == 60
    total = default_score(c)
    assert total.hard == -60
    assert total.soft == -20 * 60


def test_unassigned_contributes_nothing():
    c = make_base()
    place(c, "A", [0], 0, 0)
    talks = c.talks
    talks.append(cs.Talk(code="B", speakers=[0]))  # unassigned
    c.talks = talks
    total = cs.calculate_score(c)
    assert total.hard == 0
    assert total.soft == 0

"""Constraint spot checks: re-verifies some hand-computed cases of the C++ tests (a) through the Python bindings."""

import conference_scheduling as cs


def make_base() -> cs.ConferenceSchedule:
    """The same base instance as makeBase in cpp/test/cs_test.cpp."""

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
    b = cs.compute_breakdown(c)
    assert b.room_conflict == 60
    assert b.to_score().hard == -60000
    assert cs.calculate_score(c).hard == -60000


def test_speaker_conflict_and_pause():
    c = make_base()
    place(c, "A", [0, 1], 0, 0)
    place(c, "B", [0, 1], 1, 1)
    b = cs.compute_breakdown(c)
    assert b.speaker_conflict == 120  # 2 shared speakers x 60 overlap
    assert b.consecutive_talks_pause == 120  # overlapping pair, no pause
    assert b.to_score().hard == -(10 * 120 + 120)


def test_talk_type_compatibility_synthetic():
    c = make_base()
    c.talk_types = ["B", "L"]
    place(c, "A", [0], 0, 0, talk_type=1)  # "L" talk in "B"-only slot+room
    b = cs.compute_breakdown(c)
    assert b.talk_type_compatibility == 120
    assert b.to_score().hard == -12000000


def test_prerequisite_and_popular():
    c = make_base()
    place(c, "A", [0], 3, 2, favorite_count=10)  # 630-690, capacity 300
    place(c, "B", [1], 0, 0, favorite_count=20, prerequisite_talks=[0])
    b = cs.compute_breakdown(c)
    # A (prerequisite) ends 690 > B starts 540: violated, combined 120.
    assert b.talk_prerequisite_talks == 120
    # B is more favorited but sits in the smaller room.
    assert b.popular_talks == 120


def test_diversity_rewards():
    c = make_base()
    place(c, "A", [0], 0, 0, audience_types=["Dev"], audience_level=1)
    place(c, "B", [1], 0, 1, audience_types=["Dev"], audience_level=2, language="de")
    b = cs.compute_breakdown(c)
    assert b.audience_type_diversity == 60
    assert b.audience_level_diversity == 60
    assert b.language_diversity == 60
    score = b.to_score()
    assert score.hard == 0
    assert score.soft == 720  # 1*60 + 1*60 + 10*60


def test_speaker_makespan():
    c = make_base()
    place(c, "A", [0], 0, 0)  # day 0
    place(c, "B", [0], 5, 1)  # day 2 -> span 2 -> one 8h day
    b = cs.compute_breakdown(c)
    assert b.speaker_makespan == 480
    assert b.to_score().soft == -20 * 480


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
    b = cs.compute_breakdown(c)
    assert b.talk_required_timeslot_tags == 60
    assert b.talk_undesired_room_tags == 60
    score = b.to_score()
    assert score.hard == -60
    assert score.soft == -20 * 60


def test_unassigned_contributes_nothing():
    c = make_base()
    place(c, "A", [0], 0, 0)
    talks = c.talks
    talks.append(cs.Talk(code="B", speakers=[0]))  # unassigned
    c.talks = talks
    score = cs.calculate_score(c)
    assert score.hard == 0
    assert score.soft == 0

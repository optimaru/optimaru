"""Java quickstart `DemoDataGenerator` 재현 — cpp/src/main.cpp 와 같은 데이터.

랜덤 테마/섹터/청중/콘텐츠 태그까지 java.util.Random(0) 의 드로우 시퀀스를
그대로 따라간다 (아래 `JavaRandom`)."""

from ._core import ConferenceSchedule, Room, Speaker, Talk, Timeslot

__all__ = ["JavaRandom", "demo_conference"]

_MASK = (1 << 48) - 1

LAB = 0
BREAKOUT = 1

THEME_TAGS = ["Optimization", "AI", "Cloud"]
SECTOR_TAGS = ["Green", "Blue", "Orange"]
AUDIENCE_TAGS = ["Programmers", "Analysts", "Managers"]
CONTENT_TAGS = ["Timefold", "Constraints", "Metaheuristics", "Kubernetes"]


class JavaRandom:
    """java.util.Random 의 48-bit LCG (nextInt(bound) 만 필요한 만큼)."""

    def __init__(self, seed: int):
        self._seed = (seed ^ 0x5DEECE66D) & _MASK

    def _next(self, bits: int) -> int:
        self._seed = (self._seed * 0x5DEECE66D + 0xB) & _MASK
        return self._seed >> (48 - bits)

    def next_int(self, bound: int) -> int:
        r = self._next(31)
        m = bound - 1
        if (bound & m) == 0:  # power of two
            return (bound * r) >> 31
        u = r
        r = u % bound
        # int32 오버플로 검사(자바의 모듈로 바이어스 제거 루프).
        while ((u - r + m) & 0xFFFFFFFF) >> 31:
            u = self._next(31)
            r = u % bound
        return r


def demo_conference() -> ConferenceSchedule:
    """Java DemoDataGenerator 인스턴스: 15 talks / 6 timeslots / 5 rooms."""
    random = JavaRandom(0)

    def slot(sid, sh, sm, eh, em, talk_type, tags=()):
        return Timeslot(
            id=sid,
            start_minute=sh * 60 + sm,
            end_minute=eh * 60 + em,
            talk_types=[talk_type],
            tags=list(tags),
        )

    timeslots = [
        slot("T1", 10, 15, 12, 15, LAB),
        slot("T2", 10, 15, 11, 0, BREAKOUT),
        slot("T3", 11, 30, 12, 15, BREAKOUT),
        slot("T4", 13, 0, 15, 0, LAB, ["After lunch"]),
        slot("T5", 15, 30, 16, 15, BREAKOUT),
        slot("T6", 16, 30, 17, 15, BREAKOUT),
    ]
    rooms = [
        Room("R1", "Room A", 60, [BREAKOUT], [], ["Recorded"]),
        Room("R2", "Room B", 240, [BREAKOUT], [], []),
        Room("R3", "Room C", 630, [BREAKOUT], [], ["Recorded", "Large"]),
        Room("R4", "Room D", 70, [BREAKOUT], [], ["Recorded"]),
        Room("R5", "Room E (LAB)", 490, [LAB], [], ["Recorded"]),
    ]
    names = [
        "Amy Cole",
        "Beth Fox",
        "Carl Green",
        "Dan Jones",
        "Elsa King",
        "Flo Li",
        "Gus Poe",
        "Hugo Rye",
        "Ivy Smith",
        "Jay Watt",
        "Amy Fox",
        "Beth Green",
    ]
    speakers = [Speaker(str(k + 1), name) for k, name in enumerate(names)]
    speakers[11].undesired_timeslot_tags = ["After lunch"]  # Beth Green

    def talk(code, title, talk_type, speaker_names, level, favorites, risk):
        return Talk(
            code=code,
            title=title,
            talk_type=talk_type,
            speakers=[names.index(n) for n in speaker_names],
            theme_track_tags=[THEME_TAGS[random.next_int(3)]],
            sector_tags=[SECTOR_TAGS[random.next_int(3)]],
            audience_types=[AUDIENCE_TAGS[random.next_int(3)]],
            audience_level=level,
            content_tags=[CONTENT_TAGS[random.next_int(4)]],
            language="en",
            favorite_count=favorites,
            crowd_control_risk=risk,
        )

    # 생성(= 랜덤 드로우) 순서는 Java 제너레이터와 동일해야 한다.
    talks = [
        talk("S01", "Talk One", LAB, ["Amy Cole", "Beth Fox"], 2, 551, 1),
        talk("S02", "Talk Two", LAB, ["Carl Green"], 3, 528, 0),
    ]
    talks[0].undesired_room_tags = ["Recorded"]  # S01
    talks += [
        talk("S03", "Talk Three", BREAKOUT, ["Dan Jones"], 3, 497, 0),
        talk("S04", "Talk Four", BREAKOUT, ["Elsa King", "Flo Li"], 1, 560, 0),
        talk("S05", "Talk Five", BREAKOUT, ["Gus Poe", "Hugo Rye"], 1, 957, 0),
        talk("S06", "Talk Six", BREAKOUT, ["Ivy Smith"], 1, 957, 0),
    ]
    talks[4].prerequisite_talks = [1]  # S05 requires S02
    talks += [
        talk("S07", "Talk Seven", BREAKOUT, ["Jay Watt"], 3, 568, 0),
        talk("S08", "Talk Eight", BREAKOUT, ["Amy Fox"], 3, 183, 0),
        talk("S09", "Talk Nine", BREAKOUT, ["Beth Green", "Amy Cole"], 3, 619, 0),
        talk("S10", "Talk Ten", BREAKOUT, ["Beth Fox", "Carl Green"], 3, 603, 1),
        talk("S11", "Talk Eleven", BREAKOUT, ["Dan Jones", "Elsa King"], 1, 39, 0),
        talk("S12", "Talk Twelve", BREAKOUT, ["Flo Li", "Gus Poe"], 3, 977, 0),
    ]
    # S11 의 mutex 태그는 Java 에서 S12 생성 뒤에 드로우된다.
    talks[10].mutually_exclusive_talks_tags = [CONTENT_TAGS[random.next_int(4)]]
    talks += [
        talk("S13", "Talk Thirteen", BREAKOUT, ["Hugo Rye"], 3, 494, 0),
        talk("S14", "Talk Fourteen", BREAKOUT, ["Ivy Smith"], 3, 500, 0),
        talk("S15", "Talk Fifteen", BREAKOUT, ["Jay Watt"], 2, 658, 0),
    ]
    talks[10].required_room_tags = ["Recorded"]  # S11

    return ConferenceSchedule(
        name="Conference",
        talk_types=["Lab", "Breakout"],
        timeslots=timeslots,
        rooms=rooms,
        speakers=speakers,
        talks=talks,
    )

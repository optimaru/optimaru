from enum import IntEnum, StrEnum


class Weekday(IntEnum):
    """수업이 편성되는 요일. 값은 월요일=0 기준 인덱스."""

    MON = 0
    TUE = 1
    WED = 2
    THU = 3
    FRI = 4
    SAT = 5

    @property
    def label(self) -> str:
        return _WEEKDAY_LABELS[self]


_WEEKDAY_LABELS: dict[Weekday, str] = {
    Weekday.MON: "월",
    Weekday.TUE: "화",
    Weekday.WED: "수",
    Weekday.THU: "목",
    Weekday.FRI: "금",
    Weekday.SAT: "토",
}


class RoomType(StrEnum):
    """특별실 종류. `Room.room_type` 은 문자열이므로 여기 없는 값도 그대로 쓸 수 있다."""

    HOMEROOM = "일반교실"
    SCIENCE_LAB = "과학실"
    COMPUTER_LAB = "컴퓨터실"
    MUSIC_ROOM = "음악실"
    ART_ROOM = "미술실"
    GYM = "체육관"
    LANGUAGE_LAB = "어학실"
    HOME_ECONOMICS = "가사실"
    TECH_LAB = "기술실"

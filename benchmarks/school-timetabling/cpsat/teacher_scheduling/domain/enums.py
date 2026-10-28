from enum import IntEnum, StrEnum


class Weekday(IntEnum):
    """Weekday a lesson is scheduled on. Values are indices with Monday = 0."""

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
    Weekday.MON: "Mon",
    Weekday.TUE: "Tue",
    Weekday.WED: "Wed",
    Weekday.THU: "Thu",
    Weekday.FRI: "Fri",
    Weekday.SAT: "Sat",
}


class RoomType(StrEnum):
    """Special-room types. `Room.room_type` is a string, so values not listed here work as well."""

    HOMEROOM = "homeroom"
    SCIENCE_LAB = "science lab"
    COMPUTER_LAB = "computer lab"
    MUSIC_ROOM = "music room"
    ART_ROOM = "art room"
    GYM = "gym"
    LANGUAGE_LAB = "language lab"
    HOME_ECONOMICS = "home economics room"
    TECH_LAB = "tech room"

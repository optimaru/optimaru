"""Java quickstart 의 DemoDataGenerator 재현 — 남미 14팀, 32라운드, 182경기.

클래식 경기 추첨은 java.util.Random(0) 의 48-bit LCG 를 그대로 구현해
Java/C++ 데모와 비트 단위로 같은 인스턴스를 만든다. 유일한 차이: Java 는
LocalDate.now() 로 주말 라운드를 정하지만 여기서는 결정론을 위해 round 0
을 수요일로 고정한다 (C++ 데모와 동일).
"""

from ._core import LeagueSchedule, Match, Round, Team

__all__ = ["JavaRandom", "demo_schedule"]

_TEAM_NAMES = [
    "Cruzeiro",
    "Argentinos Jr.",
    "Boca Juniors",
    "Estudiantes",
    "Independente",
    "Racing",
    "River Plate",
    "Flamengo",
    "Gremio",
    "Santos",
    "Colo-Colo",
    "Olimpia",
    "Nacional",
    "Penharol",
]

_DISTANCE_IN_KM = [
    [0, 2163, 2163, 2160, 2156, 2156, 2163, 340, 1342, 512, 3038, 1526, 2054, 2054],
    [2163, 0, 11, 50, 813, 813, 11, 1967, 842, 1661, 1139, 1037, 202, 202],
    [2163, 11, 0, 50, 813, 813, 11, 1967, 842, 1661, 1139, 1037, 202, 202],
    [2160, 50, 50, 0, 862, 862, 50, 1957, 831, 1655, 1180, 1068, 161, 161],
    [2160, 813, 813, 862, 0, 1, 813, 2083, 1160, 1741, 910, 644, 600, 600],
    [2160, 813, 813, 862, 1, 0, 813, 2083, 1160, 1741, 910, 644, 600, 600],
    [2163, 11, 11, 50, 813, 813, 0, 1967, 842, 1661, 1139, 1037, 202, 202],
    [340, 1967, 1967, 1957, 2083, 2083, 1967, 0, 1126, 341, 2926, 1490, 1836, 1836],
    [1342, 842, 842, 831, 1160, 1160, 842, 1126, 0, 831, 1874, 820, 714, 714],
    [512, 1661, 1661, 1655, 1741, 1741, 1661, 341, 831, 0, 2589, 1151, 1545, 1545],
    [3038, 1139, 1139, 1180, 910, 910, 1139, 2926, 1874, 2589, 0, 1552, 1340, 1340],
    [1526, 1037, 1037, 1068, 644, 644, 1037, 1490, 820, 1151, 1552, 0, 1077, 1077],
    [2054, 202, 202, 161, 600, 600, 202, 1836, 714, 1545, 1340, 1077, 0, 14],
    [2054, 202, 202, 161, 600, 600, 202, 1836, 714, 1545, 1340, 1077, 14, 0],
]

_ROUNDS = 32
# Java 는 LocalDate.now() 의 토/일을 주말로 표시한다; 여기서는 round 0 을
# 수요일(0=월 .. 6=일)로 고정한다.
_FIRST_ROUND_DAY_OF_WEEK = 2


class JavaRandom:
    """java.util.Random 의 48-bit LCG (nextInt(bound) 만 구현)."""

    _MULTIPLIER = 0x5DEECE66D
    _ADDEND = 0xB
    _MASK = (1 << 48) - 1

    def __init__(self, seed: int) -> None:
        self._seed = (seed ^ self._MULTIPLIER) & self._MASK

    def _next(self, bits: int) -> int:
        self._seed = (self._seed * self._MULTIPLIER + self._ADDEND) & self._MASK
        return self._seed >> (48 - bits)

    def next_int(self, bound: int) -> int:
        if bound <= 0:
            raise ValueError("bound must be positive")
        if bound & -bound == bound:  # power of two
            return (bound * self._next(31)) >> 31
        while True:
            bits = self._next(31)
            val = bits % bound
            if bits - val + (bound - 1) < 2**31:  # Java int overflow check
                return val


def demo_schedule() -> LeagueSchedule:
    """Java DemoDataGenerator.generateDemoData() 와 같은 인스턴스."""
    random = JavaRandom(0)

    rounds = [Round(i, weekend_or_holiday=((_FIRST_ROUND_DAY_OF_WEEK + i) % 7) >= 5) for i in range(_ROUNDS)]
    teams = [Team(str(i + 1), name) for i, name in enumerate(_TEAM_NAMES)]

    matches = [
        Match(f"{i + 1}-{j + 1}", home_team=i, away_team=j)
        for i in range(len(teams))
        for j in range(len(teams))
        if i != j
    ]

    # 5% 클래식 추첨 (Java applyRandomValue: 남은 비클래식 중 nextInt(size)
    # 번째를 고르고 size 를 하나 줄인다).
    picks = int(len(matches) * 0.05)
    size = len(matches)
    for _ in range(picks):
        if size <= 0:
            break
        skip = random.next_int(size)
        for match in matches:
            if match.classic_match:
                continue
            if skip == 0:
                match.classic_match = True
                break
            skip -= 1
        size -= 1
    # 대칭 폐포: 역방향 경기가 클래식이면 클래식.
    classic_pairs = {(m.home_team, m.away_team) for m in matches if m.classic_match}
    for match in matches:
        if (match.away_team, match.home_team) in classic_pairs:
            match.classic_match = True

    return LeagueSchedule(teams, rounds, matches, _DISTANCE_IN_KM)

"""트랙2 canonical 인스턴스 생성기 (S/M/L).

benchmarks/school-timetabling/cpsat 의 `teacher_scheduling.sample` 데모 학교를 크기별로 확장한
canonical `TimetableProblem` JSON 을 만든다 (API_SPEC.md 직렬화 형태).

- 5일제: 월~목 7교시, 금 6교시 → 학급당 주 34칸
- 학급: S=6개(3학년×2반) / M=12개(3학년×4반) / L=24개(3학년×8반)
- 과목 시수표 합 33 + 창체(월1 고정, 담임) 1 = 34 → 빈칸 없이 꽉 차는 시간표
- 특별실(과학실/음악실/미술실/체육관)은 학급 수에 비례해 늘린다
- 미술은 주 1회 2연강(block_periods=2)
- 분반: 각 학년 1반의 기술가정은 기술/가정 두 반(parallel_group)
- 팀티칭: 각 학년 1반의 영어 1시간은 두 교사 동시 수업
- 불가시간: 일부 교사에게 연수/겸임 슬롯, 희망 공강일(soft)도 일부

시수 검증: 학급별 주당 시수 합 == 가용 슬롯 수, 교사별 주당 부담 ≤
min(30, max_periods_per_week) 를 확인하고, 가능하면 teacher_scheduling 의
pydantic 모델로도 검증한다.

사용:
    uv run gen_canonical.py --sizes S,M,L --out-dir out
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections import defaultdict
from pathlib import Path

# benchmarks/school-timetabling/cpsat 의 canonical pydantic 모델로 최종 검증할 때 쓴다.
_CPSAT_DIR = Path(__file__).resolve().parent / "cpsat"

# 학급 수 프리셋.
SIZES = {"S": 6, "M": 12, "L": 24}

# 과목 시수표 (창체 1시간 별도). 합계 33.
CURRICULUM: dict[str, int] = {
    "KOR": 5,
    "MATH": 5,
    "ENG": 5,
    "SCI": 4,
    "SOC": 4,
    "PE": 3,
    "MUS": 2,
    "ART": 2,
    "TECH": 2,
    "ETH": 1,
}

# 과목별 교사 1명이 맡는 최대 학급 수 (주당 부담을 30시간 밑으로 유지).
_CHUNK: dict[str, int] = {
    "KOR": 4,
    "MATH": 4,
    "ENG": 4,
    "SCI": 5,
    "SOC": 4,  # 같은 교사가 ETH 도 맡는다 (사회+도덕 5시간/학급)
    "PE": 8,
    "MUS": 10,
    "ART": 10,
    "TECH": 6,
}

_SUBJECT_NAMES = {
    "KOR": "국어",
    "MATH": "수학",
    "ENG": "영어",
    "SCI": "과학",
    "SOC": "사회",
    "PE": "체육",
    "MUS": "음악",
    "ART": "미술",
    "TECH": "기술가정",
    "ETH": "도덕",
    "HR": "창체",
}

# 요일별 교시 수 (Weekday 0=월 .. 4=금).
_PERIODS_PER_DAY = {0: 7, 1: 7, 2: 7, 3: 7, 4: 6}
_SLOTS_PER_WEEK = sum(_PERIODS_PER_DAY.values())  # 34


def _class_ids(classes: int) -> list[str]:
    """3개 학년에 균등 분배: C11, C12, ..., C21, ..."""
    per_grade = classes // 3
    return [f"C{grade}{section}" for grade in (1, 2, 3) for section in range(1, per_grade + 1)]


def generate(size: str) -> dict:
    classes = SIZES[size]
    if classes % 3 != 0:
        raise ValueError("학급 수는 3의 배수여야 한다 (3개 학년 균등 분배)")
    class_ids = _class_ids(classes)

    # --- 교사: 과목별로 학급을 chunk 단위로 잘라 배정 ---
    teachers: list[dict] = []
    teacher_of: dict[tuple[str, str], str] = {}  # (subject, class) -> teacher id
    pools: dict[str, list[str]] = defaultdict(list)

    def new_teacher(subject_ids: list[str]) -> str:
        tid = f"T{len(teachers) + 1:02d}"
        teachers.append(
            {
                "id": tid,
                "name": f"{_SUBJECT_NAMES[subject_ids[0]]}{len(pools[subject_ids[0]]) + 1}",
                "subject_ids": subject_ids,
                "unavailable": [],
                "max_periods_per_week": None,
                "max_periods_per_day": 6,
                "max_consecutive_periods": 4,
                "preferred_free_days": [],
                "homeroom_class_id": None,
            }
        )
        for s in subject_ids:
            pools[s].append(tid)
        return tid

    for subject, chunk in _CHUNK.items():
        subject_ids = ["SOC", "ETH"] if subject == "SOC" else [subject]
        count = max(2 if subject in ("TECH", "ENG") else 1, math.ceil(classes / chunk))
        pool = [new_teacher(subject_ids) for _ in range(count)]
        for i, cid in enumerate(class_ids):
            teacher_of[subject, cid] = pool[i % len(pool)]
            if subject == "SOC":
                teacher_of["ETH", cid] = pool[i % len(pool)]

    # 담임: 앞에서부터 교사 1명당 학급 1개, 창체(HR) 담당.
    by_id = {t["id"]: t for t in teachers}
    if len(teachers) < classes:
        raise ValueError(f"담임 배정 불가: 교사 {len(teachers)} < 학급 {classes}")
    for i, cid in enumerate(class_ids):
        teacher = teachers[i]
        teacher["homeroom_class_id"] = cid
        teacher["subject_ids"] = [*teacher["subject_ids"], "HR"]

    # 불가시간/희망 공강일: 4명마다 1명은 월/화 7교시 연수, 6명마다 1명은 금요일
    # 공강 희망(soft). 음악1 교사는 금요일 겸임 출강으로 종일 불가.
    for i, teacher in enumerate(teachers):
        if i % 4 == 3:
            teacher["unavailable"] = [{"day": 0, "period": 7}, {"day": 1, "period": 7}]
        if i % 6 == 5:
            teacher["preferred_free_days"] = [4]
    mus1 = by_id[pools["MUS"][0]]
    mus1["unavailable"] = [{"day": 4, "period": p} for p in range(1, _PERIODS_PER_DAY[4] + 1)]

    # --- 특별실: 학급 수 비례 ---
    rooms: list[dict] = []
    for room_type, korean, count in (
        ("SCIENCE_LAB", "과학실", max(2, math.ceil(classes / 3))),
        ("MUSIC_ROOM", "음악실", max(1, math.ceil(classes / 8))),
        ("ART_ROOM", "미술실", max(1, math.ceil(classes / 8))),
        ("GYM", "체육관", max(1, math.ceil(classes / 6))),
    ):
        for k in range(1, count + 1):
            rooms.append({"id": f"R_{room_type}{k}", "name": f"{korean}{k}", "room_type": room_type})

    subjects = [
        {"id": "KOR", "name": "국어", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
        {
            "id": "MATH",
            "name": "수학",
            "required_room_type": None,
            "preferred_periods": [1, 2, 3, 4],
            "avoided_periods": [],
        },
        {"id": "ENG", "name": "영어", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
        {
            "id": "SCI",
            "name": "과학",
            "required_room_type": "SCIENCE_LAB",
            "preferred_periods": [],
            "avoided_periods": [],
        },
        {"id": "SOC", "name": "사회", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
        {"id": "PE", "name": "체육", "required_room_type": "GYM", "preferred_periods": [], "avoided_periods": [1]},
        {
            "id": "MUS",
            "name": "음악",
            "required_room_type": "MUSIC_ROOM",
            "preferred_periods": [],
            "avoided_periods": [],
        },
        {"id": "ART", "name": "미술", "required_room_type": "ART_ROOM", "preferred_periods": [], "avoided_periods": []},
        {"id": "TECH", "name": "기술가정", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
        {"id": "ETH", "name": "도덕", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
        {"id": "HR", "name": "창체", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
    ]

    # --- 강좌 ---
    def course(cid: str, subject: str, teacher_ids: list[str], periods: int, **extra) -> dict:
        return {
            "id": extra.pop("id", f"{cid}-{subject}"),
            "class_group_id": cid,
            "subject_id": subject,
            "teacher_ids": teacher_ids,
            "periods_per_week": periods,
            "block_periods": extra.pop("block_periods", 0),
            "max_periods_per_day": extra.pop("max_periods_per_day", 1),
            "fixed_slots": extra.pop("fixed_slots", []),
            "forbidden_slots": [],
            "parallel_group": extra.pop("parallel_group", None),
        }

    split_classes = {f"C{grade}1" for grade in (1, 2, 3)}  # 각 학년 1반: 분반 + 팀티칭
    courses: list[dict] = []
    for cid in class_ids:
        for subject, hours in CURRICULUM.items():
            tid = teacher_of[subject, cid]
            if subject == "TECH" and cid in split_classes:
                # 분반: 기술/가정 두 반이 같은 시간에 진행 (교사 2명 필요).
                pool = pools["TECH"]
                other = pool[(pool.index(tid) + 1) % len(pool)]
                for suffix, teacher in (("A", tid), ("B", other)):
                    courses.append(
                        course(cid, "TECH", [teacher], hours, id=f"{cid}-TECH-{suffix}", parallel_group=f"{cid}-TECH")
                    )
                continue
            if subject == "ENG" and cid in split_classes:
                # 팀티칭: 영어 1시간은 두 교사가 함께 (본 강좌는 1시간 감축).
                pool = pools["ENG"]
                other = pool[(pool.index(tid) + 1) % len(pool)]
                courses.append(course(cid, "ENG", [tid], hours - 1))
                courses.append(course(cid, "ENG", [tid, other], 1, id=f"{cid}-ENG-TT"))
                continue
            if subject == "ART":
                # 미술은 주 1회 2연강.
                courses.append(course(cid, "ART", [tid], hours, block_periods=2))
                continue
            courses.append(course(cid, subject, [tid], hours))
        # 창체: 담임이 월요일 1교시 고정.
        homeroom = next(t["id"] for t in teachers if t["homeroom_class_id"] == cid)
        courses.append(course(cid, "HR", [homeroom], 1, id=f"{cid}-HR", fixed_slots=[{"day": 0, "period": 1}]))

    problem = {
        "calendar": {"periods_per_day": {str(d): p for d, p in _PERIODS_PER_DAY.items()}},
        "subjects": subjects,
        "teachers": teachers,
        "class_groups": [
            {"id": cid, "name": f"{cid[1]}-{int(cid[2:])}", "grade": int(cid[1]), "unavailable": []}
            for cid in class_ids
        ],
        "rooms": rooms,
        "courses": courses,
    }
    _verify(problem, size)
    return problem


def _verify(problem: dict, size: str) -> None:
    """시수 검증: 학급 주당 시수 합 == 가용 슬롯, 교사 부담 ≤ 상한."""
    per_class: dict[str, int] = defaultdict(int)
    per_teacher: dict[str, int] = defaultdict(int)
    parallel_seen: set[str] = set()
    for c in problem["courses"]:
        # 분반 그룹은 학급 칸을 한 번만 차지한다.
        key = c["parallel_group"]
        if key is None or key not in parallel_seen:
            per_class[c["class_group_id"]] += c["periods_per_week"]
            if key is not None:
                parallel_seen.add(key)
        for tid in c["teacher_ids"]:
            per_teacher[tid] += c["periods_per_week"]

    for cid, total in per_class.items():
        if total != _SLOTS_PER_WEEK:
            raise ValueError(f"[{size}] 학급 {cid} 주당 시수 {total} != 슬롯 {_SLOTS_PER_WEEK}")
    for teacher in problem["teachers"]:
        cap = min(30, teacher["max_periods_per_week"] or 30)
        unavailable = len(teacher["unavailable"])
        load = per_teacher[teacher["id"]]
        if load > cap - unavailable:
            raise ValueError(f"[{size}] 교사 {teacher['id']} 주당 {load}시간 > 상한 {cap} - 불가 {unavailable}")

    # 가능하면 canonical pydantic 모델로도 검증 (참조 무결성/분반 일관성 등).
    if str(_CPSAT_DIR) not in sys.path:
        sys.path.insert(0, str(_CPSAT_DIR))
    try:
        from teacher_scheduling import TimetableProblem
    except ImportError:
        print(f"[{size}] teacher_scheduling 미탑재 — pydantic 검증 생략", file=sys.stderr)
        return
    TimetableProblem.model_validate(problem)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sizes", default="S,M,L", help="쉼표 구분: S,M,L")
    parser.add_argument("--out-dir", default="out")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for size in args.sizes.split(","):
        size = size.strip().upper()
        problem = generate(size)
        path = out_dir / f"canonical_{size}.json"
        with open(path, "w", encoding="utf-8") as f:
            json.dump(problem, f, ensure_ascii=False, indent=2)
        print(
            f"GEN canonical_{size}: classes={len(problem['class_groups'])}"
            f" teachers={len(problem['teachers'])} rooms={len(problem['rooms'])}"
            f" courses={len(problem['courses'])} -> {path}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

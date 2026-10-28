# school-timetabling 3-way 벤치마크

같은 인스턴스를 세 솔버로 풀어 비교하는 하네스. 빌드 → 크로스체크 게이트 →
퀄리티 벤치 → 표 순서로 진행한다.

| 파일 | 역할 |
| --- | --- |
| `run_bench.py` | 전체 오케스트레이션 (트랙1 기본, `--track2` 로 canonical 트랙) |
| `evaluate.py` | 트랙1 공유 평가기: quickstart 6제약 스코어를 해에서 재계산 (세 솔버 self-report 대조) |
| `cpsat_quickstart.py` | 트랙1 CP-SAT 솔버 (6제약: hard 는 제약, soft 3종은 목적함수) |
| `gen_canonical.py` | 트랙2 canonical `TimetableProblem` S/M/L 생성기 (블록/분반/팀티칭/불가시간, 시수 검증 포함) |
| `../../use-cases/school-timetabling/cpp/bench/st_quality_bench.cpp` | 트랙1 C++ 쪽: 인스턴스 생성 + JSON 덤프 + solve + crosscheck 모드 |
| `timefold/` | Java 쪽 테스트 2개 (quickstarts 클론에 복사해 실행) + (day, period)→Timeslot 매핑 문서 |

## 트랙1: quickstart 6제약, 3-way

공통 로직은 Java quickstart 원본의 6제약(roomConflict/teacherConflict/
studentGroupConflict hard, teacherRoomStability/teacherTimeEfficiency/
studentGroupSubjectVariety soft). 이 레포 C++ 도메인의 teacherUnavailable
확장은 인스턴스에서 데이터를 비워 0 기여로 만든다.

크기 프리셋 (학급/교사/교실 — `st_quality_bench.cpp` 와 동일):
`S: 6/12/7 (204수업)`, `M: 12/20/13 (408수업)`, `L: 24/40/25 (816수업)`.
그리드는 5일 × 8교시 = 40슬롯.

```sh
# Java 트랙 준비 (1회): 클론 + mvnw 부트스트랩 (timefold/README.md)
git clone --depth 1 https://github.com/TimefoldAI/timefold-quickstarts.git /tmp/tq

# 3-way 실행 (크로스체크 게이트 → S/M 각 10초 → 재채점 → 마크다운 표)
uv run run_bench.py --sizes S,M --seconds 10 --quickstarts /tmp/tq
# Java 없이 C++ vs CP-SAT 만
uv run run_bench.py --sizes S,M --seconds 10
```

결과는 `out/RESULTS_track1.md`. 크로스체크 게이트는 C++ 와 Java 의 고정
인스턴스 스코어가 byte-identical 이어야 통과다 (`CROSSCHECK_SCORE=-3hard/-3soft`).

## 트랙2: canonical 전제약, C++ 캐노니컬 vs CP-SAT

canonical `TimetableProblem`(H1~H10 + S1~S6)을 CP-SAT(`teacher_scheduling.solve`)
와 C++ 캐노니컬 엔진(`st_timetabling.solve`, 단일 스레드 — 확장 모듈이
미빌드라 import 가 실패하면 `pending` 표기)으로 같은 time limit 에 풀고,
두 결과 모두 cpsat 의 `validate.py` 로 재채점한다.

```sh
uv run run_bench.py --track2 --sizes S,M,L --seconds 10
```

결과는 `out/RESULTS_track2.md` (status / hard 위반 수 / 기본 가중치 기준 벌점).

## 트랙3: 부분 편성 (미배정 허용)

트랙2 인스턴스에서 요일 하나를 지워(`--drop-day`, 기본 4=금) **시수 > 칸** 으로
만든 뒤 양쪽 다 H1(주당 시수)을 완화해서 푼다 — CP-SAT 은 `solve_partial`,
엔진은 같은 뜻의 조립(`weekly_period_counts hard=false weight=10000`). 전부
배정하는 것이 애초에 불가능하므로 비교 대상은 **얼마나 채웠는가**다.

```sh
uv run run_bench.py --track3 --sizes S,M --seconds 10
```

결과는 `out/RESULTS_track3.md` (배정/시수 · 미배정 · 남은 hard · 가중 벌점).

읽는 법:

- **배정/시수** 가 성적이다. 덜 채우면 가중 벌점도 같이 낮아지므로 둘을 같이 본다.
- **남은 hard** 는 미배정을 뺀 하드 위반이다. 부분해라도 배정한 부분은 하드를 전부
  지켜야 하므로 0 이 아니면 그냥 깨진 해다 — 이 칸이 이 트랙의 정합성 게이트다.
- 시간 참조(교사/학급 불가시간, 고정·금지 배치)가 지워진 요일을 가리키면 같이
  지운다. 캘린더에 없는 시간은 입력 검증이 거부하기 때문이다.

## 공정성 메모

- `st_quality_bench` 는 튜닝 스윕용 env 오버라이드를 받는다
  (`ST_BENCH_ASMOVE_WEIGHT` / `ST_BENCH_ASMOVE_MAXRUIN` /
  `ST_BENCH_NEARBY_PROB` / `ST_BENCH_LA_SIZE`). **셋팅하지 않으면 솔버
  기본값 그대로** — CI/공식 표는 항상 env 없이 돈다. 탐색 노브만 바꾸며
  채점 의미론에는 손대지 않는다.
- 세 솔버 모두 같은 인스턴스 JSON, 같은 wall-clock 예산.
- C++/Java 는 단일 스레드, CP-SAT 는 기본 8 worker 포트폴리오(`--workers`).
- moves/s: C++ 는 개선 궤적 추정(스텝당 1 move), Java 는 솔버 자체 카운터,
  CP-SAT 는 해당 없음.
- 모든 해는 자기 보고 점수가 아니라 공유 평가기(`evaluate.py` / 트랙2 는
  `validate.py`)의 재채점으로 표에 오른다.

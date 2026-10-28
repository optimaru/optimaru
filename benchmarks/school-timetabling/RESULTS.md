# school-timetabling 벤치마크

이 레포의 C++ 솔버를 실제 Timefold(Java)·OR-Tools CP-SAT 와 같은 인스턴스,
같은 wall-clock 예산으로 비교한 결과와 방법론. 벤치는
[`.github/workflows/school-timetabling-bench.yml`](../../.github/workflows/school-timetabling-bench.yml)
로 CI 에서 상시 실행되고 (push/PR/주 1회 스케줄), 하네스는
[`bench/`](./) 에 있다.

## 방법론

두 트랙으로 나눈다. 트랙1은 "동일한 여섯 제약"을 세 솔버가 얼마나 잘 푸는지
(순수 탐색 성능), 트랙2는 실전 전제약 모델에서 C++ 캐노니컬 솔버와 CP-SAT 의
품질을 잰다.

### 트랙1 — quickstart 6제약, 3-way

- **제약**: Java quickstart 원본의 여섯 제약 그대로 — roomConflict /
  teacherConflict / studentGroupConflict (hard), teacherRoomStability /
  teacherTimeEfficiency / studentGroupSubjectVariety (soft). 이 레포 C++
  도메인의 `teacherUnavailable` 확장은 인스턴스에서 데이터를 비워 0 기여로
  만든다 (공정성).
- **인스턴스**: `cpp/bench/st_quality_bench.cpp` 가 결정론적으로 생성해 JSON
  으로 덤프하고, 세 솔버가 같은 파일을 읽는다. 5일 × 8교시 = 40슬롯, 학급당
  주 34시간 커리큘럼. 프리셋 (학급/교사/교실): `S: 6/12/7 (204수업)`,
  `M: 12/20/13 (408수업)`, `L: 24/40/25 (816수업)`.
- **참가자**: (1) 이 레포 C++ 엔진(`st_quality_bench`), (2) Java Timefold
  원본 솔버(quickstarts 의 stock `TimetableConstraintProvider`,
  `benchmarks/school-timetabling/timefold/QualityBenchTest.java`), (3) OR-Tools
  CP-SAT(`bench/cpsat_quickstart.py` — hard 3종은 제약, soft 3종은 목적함수).
- **공정성 규칙**:
  - 세 솔버 모두 같은 인스턴스 JSON, 같은 wall-clock 예산 (기본 10초).
  - C++/Java 는 단일 스레드, CP-SAT 는 8 worker 포트폴리오 (`--workers` 로
    조절; 같은 예산이지만 CPU 사용량이 다르다는 점은 표 각주에 명시).
  - (day, period) → stock `Timeslot` 매핑은 09:00 + 55분×p 시작 / 45분
    수업으로, stock provider 의 "0~30분 간격" 보상 창이 C++ 의
    `|Δperiod| == 1` 인접성과 정확히 일치한다
    (`benchmarks/school-timetabling/timefold/README.md` 참고).
- **크로스체크 게이트**: 여섯 제약이 전부 비영(非零) 기여하는 고정 인스턴스
  + 고정 할당을 C++(`st_quality_bench crosscheck`) / Java stock provider /
  공유 평가기(`bench/evaluate.py`) 셋이 재채점해 **byte-identical**
  (`CROSSCHECK_SCORE=-3hard/-3soft`) 이어야 벤치를 진행한다. 상류 제약이
  바뀌면 여기서 잡힌다.
- **재채점**: 세 솔버의 해 전부 공유 평가기 `bench/evaluate.py` 로 다시
  채점해 self-report 와 대조한다. 표에는 재채점 점수가 오르고, 불일치는
  그대로 표기된다.

### 트랙2 — canonical 전제약, C++ 캐노니컬 vs CP-SAT

- **모델**: 이 레포의 canonical `TimetableProblem` (하드 H1~H10 + 소프트
  S1~S6 — 블록 연강, 분반 병합, 팀티칭, 교사 불가시간/주당·연속 상한,
  특별실 수요, 고정 배정 포함). [`API_SPEC.md`](API_SPEC.md) 계약과 동일.
- **인스턴스**: `bench/gen_canonical.py` 가 S(6학급/15교사/72강좌) /
  M(12학급/23교사/138강좌) / L(24학급/42교사/270강좌) 를 생성한다. 학급
  시수합 == 34슬롯, 교사 부담 검증, pydantic 검증 포함.
- **참가자**: (1) C++ 캐노니컬 솔버(`st_timetabling.solve`, 단일 스레드),
  (2) CP-SAT 레퍼런스(`benchmarks/school-timetabling/cpsat` 의 `teacher_scheduling.solve`,
  8 worker). 같은 time limit, 같은 seed 인자.
- **재채점**: 두 백엔드의 해 전부 CP-SAT 패키지의 `validate.py` 로 재채점
  — hard 위반 목록 + `compute_penalties` 원시 카운트 × 기본 가중치
  (20/3/30/10/5/1). **낮을수록 좋다** (트랙1 soft 와 방향이 반대).
- 두 구현의 의미론이 같다는 것 자체는 별도의 패리티 게이트
  (`python/tests/test_parity.py`, C++ `canonical_test` 의 손계산 44케이스)
  로 고정돼 있다.
- 엔진 수치는 기본 **모놀리식 인크리멘털 계산기**(fast path) 기준이다. 같은
  의미론의 플러그인 조립 경로(`st_timetabling.solve(..., constraints=)`)는
  이동 처리량이 모놀리식의 35~55% 수준이라 (track2 S/M/L, 10초 기준) 이 표의
  수치를 대표하지 않는다 — 벤치와 CI 는 항상 fast path 를 쓴다. 자세한 것은
  [`python/README.md`](python/README.md) 의 플러그인 섹션 참고.

## 최종 실측 (로컬, 2026-08-12)

환경: Apple Silicon macOS, 10초/인스턴스/솔버, seed 1, 단일 실행. Java 는
temurin 계열 JDK + timefold-quickstarts stock provider, CP-SAT 는 OR-Tools
9.15 (8 workers). 절대치는 머신에 따라 다르며 (CI 는 ubuntu-latest 4 vCPU),
단일 시드라 방향성 참고용이다.

### 트랙1 (soft: 높을수록 좋음, 전부 재채점 == self-report)

| instance | solver | final (재채점) | 최초 feasible | moves/s |
| --- | --- | --- | --- | --- |
| S: 6학급/12교사/7교실 | **cpp** | **0hard/175soft** | 0ms | 13,226,204 |
| S | cpsat | 0hard/78soft | 1566ms | n/a |
| S | java | 0hard/-312soft | 132ms | 177,738 |
| M: 12학급/20교사/13교실 | **cpp** | **0hard/348soft** | 3ms | 12,870,135 |
| M | cpsat | 0hard/-1377soft | 6424ms | n/a |
| M | java | 0hard/-2658soft | 541ms | 164,180 |
| L: 24학급/40교사/25교실 | **cpp** | **0hard/635soft** | 11ms | 12,273,466 |
| L | cpsat | n/a (10초 내 feasible 실패, UNKNOWN) | n/a | n/a |
| L | java | 0hard/-6641soft | 2242ms | 195,175 |

- moves/s: C++ 는 개선 궤적 추정(스텝당 1 move), Java 는 솔버 자체 카운터.
  CP-SAT 는 해당 없음.
- CP-SAT 는 8 worker 병렬(포트폴리오), C++/Java 는 단일 스레드.
- 이 표는 2026-08-18 의 as-move 자동 기본값 채택 **이전** 실측이다. 그
  뒤의 L cpp 는 시드 6개 평균 684.8 (645.7 대비 +6.1%), S/M 은 동일하다 —
  "튜닝 이력 요약" 참고 (단일 머신, 시드별 값이라 표의 seed 1 단일 실행과
  직접 비교는 방향성 참고용).

### 트랙2 (가중 벌점: 낮을수록 좋음, validate.py 재채점, 전부 hard 위반 0)

| instance | backend | status | hard 위반 | 가중 벌점 |
| --- | --- | --- | --- | --- |
| canonical_S | cpsat (8 workers) | FEASIBLE | 0 | 443 |
| canonical_S | **engine (1 thread)** | FEASIBLE | 0 | **310** |
| canonical_M | cpsat (8 workers) | FEASIBLE | 0 | 1511 |
| canonical_M | **engine (1 thread)** | FEASIBLE | 0 | **1176** |
| canonical_L | cpsat (8 workers) | FEASIBLE | 0 | 4164 |
| canonical_L | **engine (1 thread)** | FEASIBLE | 0 | **2752** |

- CP-SAT 는 포트폴리오 탐색이라 실행 간 변동이 있다 (S 에서 243~443 관찰).
  엔진 우위 판단은 M/L 의 큰 격차 기준 (이 런에서 M −22%, L −34%).
- 엔진의 6-시드 평균 (튜닝 확정 시점, seed 1~6): S 288.7 / M 1131.8 /
  L 2696.7 — 튜닝 전 대비 S −22.8% / M −16.7% / L −17.9%, L feasibility
  1/3 시드 → 18/18 (hard 0).

### 튜닝 이력 요약

10라운드 측정-채택/기각 튜닝의 채택분: 같은학급 nearby 선택, 하드위반 겨냥
entity recommender, soft 단계 한정 best-of-8 같은학급 스왑 무브, Late
Acceptance 크기 자동 스케일. 그 결과 트랙1 C++ soft 는 튜닝 전 대비 M 평균
+23.5%, L 평균 +79.9% (S 는 175 근처 천장 유지), moves/s 는 12.3M~13.2M
유지. 측정 프로토콜(트랙1 S/M/L × 시드 3~6개 평균, 트랙2 validate.py
재채점)은 [`bench/README.md`](README.md) 의 공정성 규칙과 같다.

2026-08-18 추가 라운드: 아래 "탐색 전략 둘" 의 as-move 를 측정 구간
[900, 1300]칸에서만 0.2/slotRoomCount 로 자동 활성하는 기본값을 채택 —
트랙1 L 시드 6개 평균 645.7 → **684.8 (+6.1%, 6승 0패)**, S/M 은 규칙이
0 을 주므로 변화 없음 (10초/시드, 단일 머신 인터리브 A/B).

### 탐색 전략 둘 — 크기에만 값을 한다 (as-move 는 측정 구간에서만 자동 ON)

vehicle-routing 에서 채택한 두 전략을 엔진의 BASIC 종류로 확장하고 여기서도
쟀다. 무브 종류가 아니라 **스텝을 쓰는 방식**이라는 점이 같다.

- `Config::descentWeight` — 수업 하나와 가까운 짝을 골라 둘의 모든 slotRoom
  과 둘의 스왑을 전수 평가해 최선을 적용하고, 개선이 없을 때까지 반복한다.
  항상 개선이라 acceptor 를 안 거친다.
- `Config::ruinRecreateAsMoveWeight` — 수업 몇 개를 ruin 하고 best-value 로
  repair 한 결과를 평범한 무브처럼 점수 매겨 acceptor 판정을 받는다. 거절되면
  되돌린다. 정체 킥과 달리 드물지도 무조건적이지도 않다.

BASIC 에서 두 전략의 단가는 **값 범위**가 정한다. 하강 한 번이 ~2 ×
slotRoomCount, destroy-repair 한 번이 ruinCount × slotRoomCount 번의 채점이라
40×25 격자에서는 한 번이 일반 스텝 수천 번어치다. 직원 근무표 도메인은 값
범위가 직원 40명이라 weight 가 0.02 자리인데 여기가 1e-4 자리인 이유가
그것이다. 처음에 vehicle-routing 의 0.01/0.05 를 그대로 넣었더니 M 에서 스텝이
1억4천만 → 1천만으로 떨어지고 soft 는 349.8 → 263.4 로 무너졌다.

예산 점유율을 맞추려면 weight × 값범위 를 일정하게 둬야 한다. 아래는 그 상수를
0.2 로 잡은 값들 — S 7e-4, M 4e-4, L 2e-4 — 로 잰 것이다. 트랙1, 20초, 시드
10개, self-report soft 평균 (높을수록 좋음), 승패는 시드별 비교:

| 크기 (수업/칸) | baseline | as-move (0.2/칸수) | 승-패-무 |
| --- | --- | --- | --- |
| S 204/280 | 175.0 | 172.9 | 0-1-9 |
| M 408/520 | 340.0 | 346.7 (+2.0%) | 5-1-4 |
| L 816/1000 | 653.2 | **680.0 (+4.1%)** | **8-2-0** |

L 만 weight 를 흔들어 보면 봉우리가 분명하다: 1e-4 668.1 (7-3), 2e-4 680.0
(8-2), 5e-4 660.3 (5-5). 하강은 같은 자리에서 5e-4 664.8 (7-3) 으로 as-move
보다 약하고, S/M 에서는 손해다.

읽는 법은 하나다. **이 트랙에서 이득은 인스턴스가 클수록 커진다 — 단,
L 급까지만.** S 는 baseline 이 이미 175 천장이라 예산을 어디로 돌리든 본전
아니면 손해고, L 은 예산 안에 천장에 못 닿으니 예산의 절반 이상을 구조적
repair 에 태우는 쪽이 이긴다 — 1000 고객 VRP 에서 나온 결론과 같은 이야기가
BASIC 에서도 나온다.

2026-08-18 재측정(Apple Silicon macOS 단일 머신, 10초/시드 6개, 같은 시드로
baseline/candidate 를 교대로 돌리는 인터리브 A/B)에서 구간의 양끝을 더 쟀다:

- 봉우리 재확인 (L 1000칸, 10초): weight 2e-4 + maxRuin 12 = **+6.1%
  (684.8 vs 645.7, 6-0-0)**. maxRuin 6 은 +1.7%, 24 는 +2.1%, weight 1e-4 는
  +4.1% — 20초/10시드에서 잡았던 2e-4/12 봉우리가 10초에서도 그대로다.
- 1200칸 (28학급/47교사/30교실, 952수업): 0.2/칸수 = +3.3% (3-0).
- **1480칸 (36학급/60교사/37교실, 1224수업): 10초 -5.5% (0-2-1), 20초 -4.8%
  (0-3)** — 예산 점유율 0.2 는 ~1300칸을 넘으면 역효과다. 5e-5 로 낮춰도
  +1.4% (2-1) 에 그친다.
- M 520칸 (10초, 시드 6개): -1.3% (3-2-1), 시드 하나가 348 → 316 으로 크게
  잃는 꼬리가 있다 (20초에서는 +2.0% 였다 — 천장 근처에서 예산만 뺏기는 것).

그래서 **하강(descent) 기본값은 0 그대로, as-move 는 측정된 구간에서만
자동으로 켠다**: `ruinRecreateAsMoveWeight` 음수(새 기본) = slotRoomCount 가
[900, 1300] 이면 0.2 / slotRoomCount, 밖이면 0. 명시적 0 = 비활성, 양수 =
그대로 사용. 구간 밖(S/M, 1480칸)은 외삽하지 않고 끈다 — 측정에서 진 곳을
기본값으로 살 이유가 없다. 캐노니컬 솔버의 같은 필드 기본은 여전히 0
(값 범위가 달라 별도 측정 전까지 자동 규칙 없음). 이 채택으로 트랙1 L 은
시드 6개 평균 645.7 → 684.8 (+6.1%, 6-0-0), S/M 은 자동 규칙이 0 을 주므로
기존과 동일 경로다.

하강이 여기서 약한 이유는 짐작이 간다. BASIC 하강이 보는 것은 수업 둘의
change/swap 전수인데, 그건 이미 일반 무브 조합이 무작위로 뽑고 있는 이웃이다 —
같은 곳을 1000 배 비싸게 보는 셈이다. 반면 as-move 의 repair 는 ruin 한 수업을
격자 전체에 대해 다시 놓으므로 일반 무브가 한 스텝에 도달하지 못하는
재배치다. 직원 근무표 도메인에서는 정확히 반대 결과가 나왔고(하강이 이기고
as-move 는 겹쳐도 안 늘었다), 거기서는 값 범위가 직원 40명뿐이라 전수 하강이
싸다는 점이 다르다.

## 실행법

### 로컬

```sh
# 0) C++ 빌드 + 테스트 게이트
cmake -S use-cases/school-timetabling/cpp -B build/st -DCMAKE_BUILD_TYPE=Release
cmake --build build/st -j
./build/st/st_test && ./build/st/canonical_test

# 1) Java 트랙 준비 (1회): quickstarts 클론 + mvnw 부트스트랩
#    (benchmarks/school-timetabling/timefold/README.md 의 절차)
git clone --depth 1 https://github.com/TimefoldAI/timefold-quickstarts.git /tmp/tq

# 2) 트랙1: 크로스체크 게이트 → 3-way → 재채점 → 표 (out/RESULTS_track1.md)
cd use-cases/school-timetabling/bench
uv run run_bench.py --sizes S,M,L --seconds 10 \
  --cpp-bin ../../../build/st/st_quality_bench --quickstarts /tmp/tq
# Java 없이 C++ vs CP-SAT 만이면 --quickstarts 생략

# 3) 트랙2 사전 조건: pybind 확장을 소스 트리에 in-place 빌드
( cd ../python && uv sync --all-groups && uv run python setup.py build_ext --inplace )

# 4) 트랙2: canonical 전제약 (out/RESULTS_track2.md)
uv run run_bench.py --track2 --sizes S,M,L --seconds 10
```

### CI

[`school-timetabling-bench.yml`](../../.github/workflows/school-timetabling-bench.yml)
이 위 순서를 그대로 실행한다: ccache 빌드 → `st_test`/`canonical_test` 게이트
→ C++/Java 크로스체크 byte-identical 게이트 → 트랙1 3-way → 트랙2 → 잡
서머리에 두 표 publish (+ 인스턴스/해 JSON 아티팩트 업로드).

- 트리거: `main` push / PR (`include/**`, `use-cases/school-timetabling/**`, `benchmarks/school-timetabling/**`,
  워크플로 자신), 주 1회 스케줄 (상류 quickstarts 드리프트 감시),
  `workflow_dispatch` (인스턴스당 예산 `seconds` 입력, 기본 10).
- 러너는 ubuntu-latest (4 vCPU) 라 절대치는 위 표와 다르다 — 게이트
  (테스트/byte-identical)는 결정론이므로 동일하게 유효하다.

## 해석 — 언제 무엇이 유리한가

- **C++ 엔진 (이 레포)**: 단일 스레드로 12~13M moves/s — 같은 메타휴리스틱
  계열인 Java Timefold(0.16~0.2M) 의 약 70배 스루풋이라, 초 단위 예산에서는
  세 크기 모두 soft 품질이 가장 좋고 first feasible 도 밀리초다 (L 도 11ms).
  JVM 웜업/의존성이 없어 콜드스타트가 즉시라, **대화형 서비스·짧은 예산·큰
  인스턴스·재수정(리스케줄링) 루프**에 유리하다. 한계: 탐색이라 최적성
  증명·bound 는 못 준다 (`FEASIBLE`/`UNKNOWN` 만).
- **Java Timefold**: 같은 제약을 선언적 ConstraintStream 으로 쓰고 생태계가
  넓지만, 이 벤치의 짧은 예산에서는 스루풋 차이가 그대로 품질 차이가 된다.
  예산을 분 단위로 늘리면 격차는 줄어든다. **기존 JVM 스택에 통합하거나
  Timefold 생태계 기능이 필요할 때** 선택.
- **CP-SAT**: 정확해 기반이라 **최적성 증명(OPTIMAL/bound)과 infeasibility
  진단이 필요할 때, 그리고 문제가 작거나 예산이 분 단위 이상일 때** 유리하다.
  이 벤치의 10초 예산에서는 S 급까지는 경쟁력이 있지만 (트랙2 S 에서 엔진과
  대등~우세 변동), 크기가 커지면 first feasible 자체가 늦다 — 트랙1 M 은
  6.4초, L 은 10초 내 실패, 트랙2 M/L 은 hard 0 은 찾지만 soft 품질이 엔진
  대비 22~34% 뒤진다. 8 worker 를 쓰고도 그렇다는 점(엔진은 1 스레드)도
  감안할 것.

요약하면: 짧은 예산·큰 문제·서비스 임베딩은 C++ 엔진, 증명이 필요한 소규모
확정 스케줄은 CP-SAT, JVM 통합·생태계는 Java. 세 구현의 채점 의미론이 서로
일치한다는 것은 크로스체크/패리티 게이트가 CI 에서 계속 보증한다.

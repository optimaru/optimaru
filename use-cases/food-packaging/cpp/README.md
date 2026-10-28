# Food Packaging — C++ 포트

Timefold quickstart `food-packaging` 을 `include/optimaru/`(header-only 엔진) 위에
포팅한 것. 파일 구성:

- `include/fp/domain.hpp` — 도메인 구조체, HardMediumSoft↔HardSoft 점수
  매핑(`kMediumUnit = 10^10`), LIST `SolutionDescriptor` 특수화
  (pinned prefix 를 엔진 뷰에서 숨기는 위치 변환 포함)
- `include/fp/score_calculator.hpp` — full recompute 기준 구현
  (`prepare` / `updateShadowVariables` / `calculateScore` / `analyze`)
- `include/fp/incremental_calculator.hpp` — before/after LIST 훅 기반
  O(변경 라인 길이) 증분 계산기 (shadow 체인을 라인 단위로 재파생)
- `include/fp/solver.hpp` — 엔진 래퍼. `Line.operator`(Java 의 두 번째
  planning variable)는 엔진 레그 사이 힐클라이밍으로 교대 최적화
- `src/demo_data.cpp` — Java `DemoDataGenerator` 재현
  (`java.util.Random(37)` 비트 동일 레플리카; 월요일 00:00 == 0분)
- `src/main.cpp` — 데모 실행 (기본 10초, hard 0 미도달 시 non-zero exit)
- `test/fp_test.cpp` — 제약별 손계산 케이스, 랜덤 LIST 무브 2000회
  incremental==full 교차검증, FULL_ASSERT solve, pinning/pool/operator/
  데모 검증

## 빌드

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/fp_test && ./build/fp_demo
```

## 의미론 주의점 (Java 대비)

- `minStartTime` 과 `priority` 는 Java 와 동일하게 **어떤 제약도 읽지
  않는** 데이터 필드다 (quickstart 원본의 특성 그대로).
- 첫 작업은 라인 시작 시각에 길이 0 의 청소 구간을 갖고 바로 생산을
  시작한다 (Java shadow supplier 의 동작 그대로).
- 같은 라인 위 작업쌍도 operator 를 공유하지만 청소 구간이 순차적이라
  절대 겹치지 않는다 — full 계산기는 쌍을 그대로 순회하고, 증분
  계산기는 라인 간 쌍만 유지한다 (동일 의미).
- operator 가 없는 라인들(Java 의 `operator == null`)은 서로 같은
  그룹으로 묶인다 (Java `Joiners.equal` 의 null 매칭과 동일).
- caveat: `Line.operator` 는 Timefold 처럼 무브 단위로 공동 탐색되지
  않고 엔진 레그 사이에 재배정된다. operator 수 >= 라인 수면 단사
  배정으로 충돌 제약이 항상 0 이라 결과가 완전히 동치이고(데모가 이
  경우), operator 수 < 라인 수면 교대 휴리스틱이다.
- pinned 작업은 라인의 **prefix** 여야 한다 (`prepare` 가 검증).
  quickstart 의 continuous planning 은 "지금 이전" 작업을 고정하므로
  항상 prefix 다.

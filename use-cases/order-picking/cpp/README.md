# Order picking (C++)

Timefold quickstart [order-picking](https://github.com/TimefoldAI/timefold-quickstarts)
포팅: 제네릭 엔진(`../../../include`, header-only) 위에 도메인/제약만 구현했다.

- `include/op/domain.hpp` — 창고 레이아웃(선반 5열 x 3행, 좌우면, 행 0~10)과
  `calculateDistance`, 도메인 구조체, LIST kind `SolutionDescriptor` 특수화
  (트롤리의 `pickTaskIndices` 가 `@PlanningListVariable`).
- `include/op/score_calculator.hpp` — full recompute + shadow 변수
  (`trolleyIndex`/`previousPickTask`/`nextPickTask`) 재계산 + 제약별 breakdown.
- `include/op/incremental_calculator.hpp` — before/afterListChanged 훅으로
  트롤리 단위 O(리스트) 증분 채점 (FULL_ASSERT 로 full recompute 와 대조 검증).
- `include/op/solver.hpp` — 엔진 래퍼 (LIST 무브 믹스 + ALNS + 창고 거리 nearby).
- `src/demo_data.cpp` — Java `DemoDataGenerator` 재현 (`java.util.Random(37)`
  비트 동일 재생: 37개 상품 카탈로그, 8주문, 트롤리 5대 x 버킷 4 x 48000cm3).

## 빌드 / 실행

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/op_test   # 제약별 손계산 검증 + incremental==full + FULL_ASSERT solve
./build/op_demo   # Java 데모 데이터셋 10초 solve, feasible(hard 0) 도달
```

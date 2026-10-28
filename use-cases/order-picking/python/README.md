# order_picking (Python)

Timefold quickstart **order-picking** 의 C++ 포팅( `../cpp`, 제네릭 엔진 `include/optimaru/` 사용)을
pybind11 로 감싼 Python 패키지.

## 문제

창고(선반 5열 A~E x 3행, 좌/우면 x 행 0~10)에서 주문 아이템들을 트롤리에 담아 피킹한다.

- **planning entity**: `Trolley` — `pick_task_indices` 가 `@PlanningListVariable`
  (트롤리가 방문 순서대로 담을 `PickTask` 인덱스 리스트).
- **shadow 변수**: `PickTask.trolley_index / previous_pick_task / next_pick_task`
  (Java 의 `@InverseRelationShadowVariable` / `@PreviousElementShadowVariable` /
  `@NextElementShadowVariable` 대응) — 리스트에서 파생되며 `calculate_score` /
  `solve` 가 항상 최신으로 맞춰 준다.

## 제약 (Java `OrderPickingConstraintProvider` 전수 이식)

| 레벨 | 제약 | 내용 |
| --- | --- | --- |
| hard | requiredNumberOfBuckets | 주문별 필요 버킷 수(`ceil(주문 부피 / 버킷 용량)`, 주문끼리 버킷 공유 없음) 합이 트롤리 버킷 수를 넘으면 초과분만큼 감점 |
| soft | minimizeDistanceFromPreviousPickTask | 직전 픽(첫 픽이면 트롤리 출발점)에서의 창고 거리 |
| soft | minimizeDistanceFromLastPickTaskToPathOrigin | 마지막 픽에서 출발점으로 돌아오는 거리 |
| soft | minimizeOrderSplitByTrolley | 주문이 걸친 트롤리 수(countDistinct) x 1000 |

점수 매핑: Java `HardSoftScore`(int) → 엔진 `HardSoftScore(int64 hard, double soft)`
1:1 (hard = -버킷 부족분, soft = -(이동 거리 + 1000 x 주문 분할 수)). 별도 레벨 접기 없음.

## 설치 / 테스트

```bash
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

## 사용

```python
import order_picking as op

solution = op.build_demo_data()   # Java DemoDataGenerator 데이터셋 (Random(37) 비트 동일 재현)
op.solve(solution, time_limit_seconds=5.0, random_seed=0)
print(solution.score)             # 예: 0hard/-8652soft
print(solution.routes())          # {트롤리 id: [pick task id, ...]}
```

JSON in/out 은 `order_picking.models` (pydantic v2, 선택 의존성):

```python
from order_picking.models import OrderPickingRequest, solve_request

response = solve_request(OrderPickingRequest.model_validate(request_dict))
print(response.feasible, response.distance_by_trolley)
```

`SolverConfig` 로 엔진 노브(시간 예산, LIST 무브 가중치, ALNS, nearby, FULL_ASSERT,
`parallel_solver_count` 멀티스타트 등)를 조절할 수 있다 — `_core.pyi` 참고.

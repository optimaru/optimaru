"""order_picking.constraints — 비용 함수를 선언으로 고르는 API.

"어떤 제약을 켤지" 를 고르는 선택 경로만 본다. 제약별 채점값은
`test_order_picking.py` 와 C++ 테스트가 맡는다.
"""

import pytest

import order_picking as op
from order_picking.constraints import (
    Constraint,
    MinimizeOrderSplitByTrolley,
    get_constraints,
    minimize_distance_from_last_pick_task_to_path_origin,
    minimize_distance_from_previous_pick_task,
    minimize_order_split_by_trolley,
    required_number_of_buckets,
    score,
)


def build_solution():
    return op.build_demo_data()


def solve(constraints=None):
    solution = build_solution()
    op.solve(solution, time_limit_seconds=0.5, random_seed=1, constraints=constraints)
    return solution.score


def test_list_constraints_matches_the_spec_names():
    """스펙이 들고 있는 이름은 코어가 실제로 아는 이름이어야 한다."""
    available = op.list_constraints()
    assert available == [
        "required_number_of_buckets",
        "minimize_distance_from_previous_pick_task",
        "minimize_distance_from_last_pick_task_to_path_origin",
        "minimize_order_split_by_trolley",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == op.list_constraints()


def test_default_constraints_reproduce_the_monolithic_cost_function():
    """기본 스펙 전부로 조립한 비용 함수는 모놀리식 채점과 같은 값을 내야 한다.

    두 경로가 같은 **해**에 도달하는지로는 볼 수 없다. 플러그인 경로에는
    order_consolidate_moves 연산자가 걸리지 않기 때문이다 (모놀리식 계산기의
    라이브 볼륨 표를 읽는다 — cpp/include/op/solver.hpp)."""
    solution = build_solution()
    op.solve(
        solution,
        time_limit_seconds=0.5,
        random_seed=1,
        constraints=get_constraints(),
    )
    recomputed = op.calculate_score(solution)
    assert solution.score.hard == recomputed.hard
    assert solution.score.soft == pytest.approx(recomputed.soft)


def test_picking_a_subset_drops_the_other_terms():
    """목록에 없는 제약은 채점되지 않는다 — 분할 항이 통째로 사라진다."""
    baseline = solve()
    assert baseline.soft < 0

    distance_only = solve(
        [
            required_number_of_buckets,
            minimize_distance_from_previous_pick_task,
            minimize_distance_from_last_pick_task_to_path_origin,
        ]
    )
    # 분할 벌점(가중치 1000)이 빠졌으니 소프트가 훨씬 덜 음수다.
    assert distance_only.soft > baseline.soft


def test_reweighting_one_constraint_scales_only_that_term():
    """분할 가중치만 올리면 소프트만 커지고 하드는 그대로."""
    picks = [required_number_of_buckets]
    base = solve([*picks, minimize_order_split_by_trolley])
    heavier = solve([*picks, MinimizeOrderSplitByTrolley(weight=100_000)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # 벌점이므로 더 음수


def test_level_can_be_flipped():
    """소프트 제약을 하드로 올리면 그 벌점이 하드 레벨로 옮겨간다.

    모든 피킹을 어딘가에는 실어야 하므로 (주문, 트롤리) 짝은 0 이 될 수
    없다 — 레벨을 올리면 반드시 하드에 잡힌다."""
    soft_level = solve([required_number_of_buckets, minimize_order_split_by_trolley])
    assert soft_level.hard == 0 and soft_level.soft < 0

    hard_level = solve([required_number_of_buckets, MinimizeOrderSplitByTrolley(hard=True)])
    assert hard_level.hard < 0
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """오타가 비용 함수를 조용히 그대로 두면 안 된다."""

    class Typo:
        name = "minimize_order_split_by_trolleys"  # 뒤에 s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """빈 목록은 '아무것도 채점하지 않음'이라 실수일 가능성이 높다."""
    with pytest.raises(ValueError, match="must not be empty"):
        op.solve(build_solution(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """모듈 상위 인스턴스는 기본값 그대로여야 한다 (weight=None)."""
    assert minimize_order_split_by_trolley.weight is None
    assert minimize_order_split_by_trolley.hard is None
    assert minimize_order_split_by_trolley.exponent == 1.0


# --- 순수 파이썬 채점 (constraint_costs -> score) ---------------------------


def _raw():
    """한 번 푼 해의 제약별 원본 cost."""
    solution = build_solution()
    op.solve(solution, time_limit_seconds=0.4, random_seed=1)
    return op.constraint_costs(solution), solution


def test_score_reproduces_the_engine_score_exactly():
    """파이썬 채점은 엔진과 hard/soft 가 정확히 같아야 한다.

    이 도메인은 레벨이 둘뿐이라 접힘 없이 그대로 견줄 수 있다. 이 단언이
    `score()` 가 엔진의 applyRaw 산술(가중치·셰이핑·unit 별 하드 반올림)을
    제대로 재현하는지 이 패키지에서 고정한다."""
    raw, solution = _raw()
    engine = op.calculate_score(solution)
    python = score(raw)
    assert python.hard == engine.hard
    assert python.soft == pytest.approx(engine.soft)


def test_score_is_additive_over_the_picked_constraints():
    """전체 점수는 제약별 점수의 합이다 — 목록에 없는 항은 그대로 빠진다.

    하드 반올림이 unit 기여분마다 일어나므로 제약별로 나눠 더해도 값이
    같다. "이 항이 점수에 얼마를 보태는가" 를 재채점 없이 읽을 수 있다."""
    raw, _ = _raw()
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_score_reweighting_is_linear_without_rescoring():
    """가중치만 바꾸면 그 항이 선형으로 스케일한다 — 재채점 없이."""
    raw, _ = _raw()
    base = score(raw, [minimize_order_split_by_trolley])
    ten = score(raw, [MinimizeOrderSplitByTrolley(weight=10)])
    default_weight = raw[minimize_order_split_by_trolley.name]["weight"]
    factor = 10 / default_weight
    if base.hard:
        assert ten.hard == pytest.approx(base.hard * factor, rel=1e-6)
    else:
        assert ten.soft == pytest.approx(base.soft * factor, rel=1e-9)


def test_score_rejects_an_unknown_constraint_name():
    """이름이 틀린 스펙(예: 이름 바뀐 컴포넌트)은 조용히 무시되면 안 된다."""
    raw, _ = _raw()

    class Renamed(Constraint):
        name = "definitely_not_a_constraint"

    with pytest.raises(ValueError, match="unknown constraint"):
        score(raw, [Renamed()])

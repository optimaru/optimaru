"""PDP geometry objectives, independently hand-calculated."""

import math

import pdp_routing as pdp
import pytest
from pdp_routing.constraints import MinimizeEnclosingCircleRadius, score

EARTH_RADIUS = 6_371_000.0


def _plan(points, *, depot=(0.0, 0.0), max_route_radius=0.0):
    locations = [pdp.Location(*depot), *(pdp.Location(*point) for point in points)]
    nodes = []
    deliveries = []
    for delivery_index in range((len(points) + 1) // 2):
        pickup = 2 * delivery_index
        dropoff = min(pickup + 1, len(points) - 1)
        nodes.append(
            pdp.Node(
                f"P{delivery_index}", pickup + 1, 0, 100_000, 0, delivery_index, True
            )
        )
        nodes.append(
            pdp.Node(
                f"D{delivery_index}", dropoff + 1, 0, 100_000, 0, delivery_index, False
            )
        )
        deliveries.append(pdp.Delivery(str(delivery_index), pickup, pickup + 1, 1))
    vehicle = pdp.Vehicle("V", 100, 0, max_route_radius_meters=max_route_radius)
    vehicle.node_indices = list(range(len(nodes)))
    plan = pdp.PdpRoutePlan("geometry", locations, [vehicle], nodes, deliveries)
    plan.rebuild_unassigned_pool()
    return plan


def _cost(plan, name="minimize_enclosing_circle_radius"):
    return sum(pdp.constraint_costs(plan)[name]["costs"])


def test_duplicate_pair_has_zero_radius():
    assert _cost(_plan([(1.0, 1.0), (1.0, 1.0)])) == pytest.approx(0.0)


def test_two_stops_use_half_their_distance():
    expected = EARTH_RADIUS * math.radians(0.01)
    assert _cost(_plan([(0.0, 0.0), (0.0, 0.02)])) == pytest.approx(expected)


def test_right_triangle_with_duplicate_fourth_point():
    points = [(0.0, 0.0), (0.0, 0.02), (0.02, 0.0), (0.0, 0.0)]
    mean_latitude = math.radians(sum(point[0] for point in points) / len(points))
    east = EARTH_RADIUS * math.radians(0.02) * math.cos(mean_latitude)
    north = EARTH_RADIUS * math.radians(0.02)
    expected = math.hypot(east, north) / 2
    assert _cost(_plan(points)) == pytest.approx(expected)


def test_depot_radius_is_distinct_and_weight_exponent_are_manual():
    plan = _plan([(0.0, 1.0), (0.0, 1.001)], max_route_radius=1.0)
    radius = EARTH_RADIUS * math.radians(0.0005)
    assert _cost(plan) == pytest.approx(radius)
    assert _cost(plan, "max_route_radius") > 100_000

    raw = pdp.constraint_costs(plan)
    shaped = score(raw, [MinimizeEnclosingCircleRadius(weight=3, exponent=2)])
    assert shaped.hard == 0
    assert shaped.soft == pytest.approx(-3 * radius**2)

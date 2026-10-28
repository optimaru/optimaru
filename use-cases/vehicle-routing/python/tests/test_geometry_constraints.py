"""Geometry constraints with coordinates whose answers are calculable by hand."""

import math

import pytest
import vrp_routing as vrp
from vrp_routing.constraints import MinimizeEnclosingCircleRadius, score

EARTH_RADIUS = 6_371_000.0


def _plan(points, *, depot=(0.0, 0.0), max_route_radius=0.0):
    locations = [vrp.Location(*depot), *(vrp.Location(*point) for point in points)]
    visits = [
        vrp.Visit(str(i), str(i), i + 1, 1, 0, 100_000, 0) for i in range(len(points))
    ]
    vehicle = vrp.Vehicle("V", 100, 0, max_route_radius_meters=max_route_radius)
    vehicle.visit_indices = list(range(len(points)))
    plan = vrp.VehicleRoutePlan("geometry", locations, [vehicle], visits)
    plan.rebuild_unassigned_pool()
    return plan


def _cost(plan, name="minimize_enclosing_circle_radius"):
    return sum(vrp.constraint_costs(plan)[name]["costs"])


@pytest.mark.parametrize("points", [[], [(1.0, 1.0)], [(1.0, 1.0), (1.0, 1.0)]])
def test_zero_radius_for_fewer_than_two_distinct_points(points):
    assert _cost(_plan(points)) == pytest.approx(0.0)


def test_two_points_and_collinear_middle_use_half_the_endpoint_distance():
    # At the equator, longitude maps to R * radians(lon). The minimum circle
    # around 0.00° and 0.02° therefore has radius R * radians(0.01°).
    expected = EARTH_RADIUS * math.radians(0.01)
    assert _cost(_plan([(0.0, 0.0), (0.0, 0.02)])) == pytest.approx(expected)
    assert _cost(_plan([(0.0, 0.0), (0.0, 0.01), (0.0, 0.02)])) == pytest.approx(
        expected
    )


def test_right_triangle_uses_half_its_hypotenuse():
    # The right angle is at (0, 0), so the hypotenuse is the diameter.
    points = [(0.0, 0.0), (0.0, 0.02), (0.02, 0.0)]
    mean_latitude = math.radians(sum(point[0] for point in points) / len(points))
    east = EARTH_RADIUS * math.radians(0.02) * math.cos(mean_latitude)
    north = EARTH_RADIUS * math.radians(0.02)
    expected = math.hypot(east, north) / 2
    assert _cost(_plan(points)) == pytest.approx(expected)


def test_enclosing_circle_is_not_depot_radius_and_shaping_is_exact():
    # Visits are only ~111 m apart near 1°E, while the depot is ~111 km away.
    # The enclosing circle sees only the visits; max_route_radius sees depot.
    plan = _plan([(0.0, 1.0), (0.0, 1.001)], max_route_radius=1.0)
    radius = EARTH_RADIUS * math.radians(0.0005)
    assert _cost(plan) == pytest.approx(radius)
    assert _cost(plan, "max_route_radius") > 100_000

    raw = vrp.constraint_costs(plan)
    shaped = score(raw, [MinimizeEnclosingCircleRadius(weight=2, exponent=2)])
    assert shaped.hard == 0
    assert shaped.soft == pytest.approx(-2 * radius**2)

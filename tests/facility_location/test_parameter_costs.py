"""Facility location has no parameter that acts only through a cost: every such field is required."""

import optimaru.facility_location as fl
from optimaru.facility_location.constraints import FacilityCapacityCost
from optimaru.facility_location.models import FacilityLocationProblemModel, parameter_uses


def test_no_parameter_needs_a_cost():
    model = FacilityLocationProblemModel.model_validate(
        {
            "facilities": [
                {"id": "F0", "location": {"latitude": 51.5, "longitude": -0.1}, "setup_cost": 500, "capacity": 20},
                {"id": "F1", "location": {"latitude": 51.4, "longitude": -0.0}, "setup_cost": 300, "capacity": 20},
            ],
            "consumers": [{"id": "C0", "location": {"latitude": 51.52, "longitude": -0.12}, "demand": 5}],
        }
    )
    assert parameter_uses(model) == []
    fl.solve(model, time_limit_seconds=0, constraints=[FacilityCapacityCost()])

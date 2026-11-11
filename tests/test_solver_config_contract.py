"""The part of SolverConfig every use case shares (optimaru::python::bind_solver_config)."""

import copy

import pytest

import optimaru

from ._build import use_case_module

SHARED_FIELDS = [
    "spent_limit_seconds",
    "unimproved_spent_limit_seconds",
    "step_count_limit",
    "unimproved_step_count_limit",
    "random_seed",
    "parallel_solver_count",
    "late_acceptance_size",
    "full_assert",
]
# No automatic late-acceptance size: 0 is refused there, and means "auto" everywhere else.
NO_AUTO_LATE_ACCEPTANCE = {"employee_scheduling"}


def _config(use_case):
    return use_case_module(use_case).SolverConfig()


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_the_shared_fields_exist_and_copy(use_case):
    config = _config(use_case)
    for name in SHARED_FIELDS:
        assert hasattr(config, name), name
    config.random_seed = 123
    config.full_assert = True
    config.step_count_limit = 7
    config.unimproved_step_count_limit = 5
    config.spent_limit_seconds = 1.5
    clone = copy.copy(config)
    assert (clone.random_seed, clone.full_assert) == (123, True)
    assert (clone.step_count_limit, clone.unimproved_step_count_limit, clone.spent_limit_seconds) == (7, 5, 1.5)


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_the_shared_fields_refuse_the_same_values_with_the_same_words(use_case):
    config = _config(use_case)
    with pytest.raises(ValueError, match="spent_limit_seconds must be finite and >= 0"):
        config.spent_limit_seconds = -1
    with pytest.raises(ValueError, match="unimproved_spent_limit_seconds must be at most 1e9 seconds"):
        config.unimproved_spent_limit_seconds = 1e10
    with pytest.raises(ValueError, match="parallel_solver_count must be positive"):
        config.parallel_solver_count = 0
    if use_case in NO_AUTO_LATE_ACCEPTANCE:
        with pytest.raises(ValueError, match="late_acceptance_size must be positive"):
            config.late_acceptance_size = 0
    else:
        config.late_acceptance_size = 0
        assert config.late_acceptance_size == 0
        with pytest.raises(ValueError, match=r"late_acceptance_size must be >= 0 \(0 = auto\)"):
            config.late_acceptance_size = -1
    if hasattr(config, "ruin_recreate_rewards"):
        with pytest.raises(ValueError, match="ruin_recreate_rewards must contain exactly 4 values"):
            config.ruin_recreate_rewards = [1.0]


# Their SolverConfig lets the caller choose the acceptance rule (optimaru::AcceptanceConfig).
# Pickup and delivery chooses it too, on fields of its own.
ACCEPTANCE_USE_CASES = ["quadratic_assignment", "semiconductor_scheduling", "vehicle_routing"]
DEFAULT_RULE = {"semiconductor_scheduling": "SIMULATED_ANNEALING"}


@pytest.mark.parametrize("use_case", ACCEPTANCE_USE_CASES)
def test_the_acceptance_rules_and_their_parameters(use_case):
    module = use_case_module(use_case)
    assert set(module.AcceptorType.__members__) == {
        "HILL_CLIMBING",
        "LATE_ACCEPTANCE",
        "ENTITY_TABU",
        "SIMULATED_ANNEALING",
        "GREAT_DELUGE",
        "STEP_COUNTING_HILL_CLIMBING",
        "RECORD_TO_RECORD_TRAVEL",
    }
    config = module.SolverConfig()
    assert config.acceptor_type == getattr(module.AcceptorType, DEFAULT_RULE.get(use_case, "LATE_ACCEPTANCE"))
    assert config.entity_tabu_size == 7
    assert config.simulated_annealing_starting_temperature == (0, 0.0)
    assert config.simulated_annealing_temperature_fraction == pytest.approx(0.01)
    assert config.great_deluge_water_level_step == (0, 0.0)
    assert config.step_counting_hill_climbing_size == 400
    assert (config.record_to_record_start_gap, config.record_to_record_end_gap) == (0.02, 0.0)

    config.acceptor_type = module.AcceptorType.GREAT_DELUGE
    config.entity_tabu_size = 3
    config.simulated_annealing_starting_temperature = (1, 20.0)
    config.simulated_annealing_temperature_fraction = 0.05
    config.great_deluge_water_level_step = (0, 0.5)
    config.step_counting_hill_climbing_size = 50
    config.record_to_record_start_gap = 0.1
    clone = copy.copy(config)
    assert clone.acceptor_type == module.AcceptorType.GREAT_DELUGE
    assert (clone.entity_tabu_size, clone.step_counting_hill_climbing_size) == (3, 50)
    assert clone.simulated_annealing_starting_temperature == (1, 20.0)
    assert clone.simulated_annealing_temperature_fraction == pytest.approx(0.05)
    assert clone.great_deluge_water_level_step == (0, 0.5)
    assert clone.record_to_record_start_gap == pytest.approx(0.1)


@pytest.mark.parametrize("use_case", ACCEPTANCE_USE_CASES)
@pytest.mark.parametrize(
    ("name", "value", "message"),
    [
        ("entity_tabu_size", -1, "entity_tabu_size must be >= 0"),
        ("simulated_annealing_starting_temperature", (-1, 0.0), "must be finite and >= 0"),
        ("simulated_annealing_starting_temperature", (0, float("inf")), "must be finite and >= 0"),
        ("simulated_annealing_temperature_fraction", 0, "must be finite and > 0"),
        ("simulated_annealing_temperature_fraction", float("inf"), "must be finite and > 0"),
        ("great_deluge_water_level_step", (0, -0.5), "great_deluge_water_level_step must be finite and >= 0"),
        ("great_deluge_water_level_step", (0, float("nan")), "great_deluge_water_level_step must be finite and >= 0"),
        ("step_counting_hill_climbing_size", 0, "step_counting_hill_climbing_size must be positive"),
    ],
)
def test_the_acceptance_parameters_refuse_the_same_values_with_the_same_words(use_case, name, value, message):
    config = use_case_module(use_case).SolverConfig()
    with pytest.raises(ValueError, match=message):
        setattr(config, name, value)

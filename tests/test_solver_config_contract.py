"""The part of SolverConfig every use case shares (optimaru::python::bind_solver_config)."""

import copy

import pytest

import optimaru

from ._build import use_case_module

SHARED_FIELDS = [
    ("stop", "time_limit_seconds"),
    ("stop", "unimproved_time_limit_seconds"),
    ("stop", "step_limit"),
    ("stop", "unimproved_step_limit"),
    ("run", "random_seed"),
    ("run", "parallel_solver_count"),
    ("acceptance", "late_acceptance_size"),
    ("run", "full_assert"),
]
# No automatic late-acceptance size: 0 is refused there, and means "auto" everywhere else.
NO_AUTO_LATE_ACCEPTANCE = {"employee_scheduling"}


def _config(use_case):
    return use_case_module(use_case).SolverConfig()


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_the_shared_fields_exist_and_copy(use_case):
    config = _config(use_case)
    for group, name in SHARED_FIELDS:
        assert name in dir(getattr(config, group)), f"{group}.{name}"
    config.run.random_seed = 123
    config.run.full_assert = True
    config.stop.step_limit = 7
    config.stop.unimproved_step_limit = 5
    config.stop.time_limit_seconds = 1.5
    clone = copy.copy(config)
    assert (clone.run.random_seed, clone.run.full_assert) == (123, True)
    assert (clone.stop.step_limit, clone.stop.unimproved_step_limit, clone.stop.time_limit_seconds) == (7, 5, 1.5)


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_the_shared_fields_refuse_the_same_values_with_the_same_words(use_case):
    config = _config(use_case)
    with pytest.raises(ValueError, match=r"stop\.time_limit_seconds must be finite and >= 0"):
        config.stop.time_limit_seconds = -1
    with pytest.raises(ValueError, match=r"stop\.unimproved_time_limit_seconds must be at most 1e9 seconds"):
        config.stop.unimproved_time_limit_seconds = 1e10
    with pytest.raises(ValueError, match=r"run\.parallel_solver_count must be positive"):
        config.run.parallel_solver_count = 0
    if use_case in NO_AUTO_LATE_ACCEPTANCE:
        with pytest.raises(ValueError, match=r"acceptance\.late_acceptance_size must be positive"):
            config.acceptance.late_acceptance_size = 0
    else:
        config.acceptance.late_acceptance_size = 0
        assert config.acceptance.late_acceptance_size == 0
        with pytest.raises(ValueError, match=r"acceptance\.late_acceptance_size must be >= 0 \(0 = auto\)"):
            config.acceptance.late_acceptance_size = -1
    if "alns" in config.GROUPS and "rewards" in dir(config.alns):
        with pytest.raises(ValueError, match=r"alns\.rewards must contain exactly 4 values"):
            config.alns.rewards = [1.0]


# Their SolverConfig lets the caller choose the acceptance rule (optimaru::AcceptanceConfig).
ACCEPTANCE_USE_CASES = ["pickup_delivery", "quadratic_assignment", "semiconductor_scheduling", "vehicle_routing"]
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
    assert config.acceptance.rule == getattr(module.AcceptorType, DEFAULT_RULE.get(use_case, "LATE_ACCEPTANCE"))
    assert config.acceptance.entity_tabu_size == 7
    assert config.acceptance.simulated_annealing_starting_temperature == (0, 0.0)
    assert config.acceptance.simulated_annealing_temperature_fraction == pytest.approx(0.01)
    assert config.acceptance.great_deluge_water_level_step == (0, 0.0)
    assert config.acceptance.step_counting_hill_climbing_size == 400
    assert (config.acceptance.record_to_record_start_gap, config.acceptance.record_to_record_end_gap) == (0.02, 0.0)

    config.acceptance.rule = module.AcceptorType.GREAT_DELUGE
    config.acceptance.entity_tabu_size = 3
    config.acceptance.simulated_annealing_starting_temperature = (1, 20.0)
    config.acceptance.simulated_annealing_temperature_fraction = 0.05
    config.acceptance.great_deluge_water_level_step = (0, 0.5)
    config.acceptance.step_counting_hill_climbing_size = 50
    config.acceptance.record_to_record_start_gap = 0.1
    clone = copy.copy(config)
    assert clone.acceptance.rule == module.AcceptorType.GREAT_DELUGE
    assert (clone.acceptance.entity_tabu_size, clone.acceptance.step_counting_hill_climbing_size) == (3, 50)
    assert clone.acceptance.simulated_annealing_starting_temperature == (1, 20.0)
    assert clone.acceptance.simulated_annealing_temperature_fraction == pytest.approx(0.05)
    assert clone.acceptance.great_deluge_water_level_step == (0, 0.5)
    assert clone.acceptance.record_to_record_start_gap == pytest.approx(0.1)


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
        ("record_to_record_start_gap", -0.1, "record_to_record_start_gap must be finite and >= 0"),
        ("record_to_record_end_gap", float("nan"), "record_to_record_end_gap must be finite and >= 0"),
    ],
)
def test_the_acceptance_parameters_refuse_the_same_values_with_the_same_words(use_case, name, value, message):
    config = use_case_module(use_case).SolverConfig()
    with pytest.raises(ValueError, match=message):
        setattr(config.acceptance, name, value)

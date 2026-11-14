"""Solving on the optimaru API (optimaru/_api.py): one setting moves solve() and solve_request() there.

The API is a stand-in on a local port (tests/_api_server.py) that solves with this checkout the
way the hosted API does, so what the client sends is checked against what a server reads back.
"""

import importlib
import json
import socket
import warnings

import pytest

import optimaru
from optimaru import _api

from ._api_server import MODEL_USE_CASES, REQUEST_USE_CASES, ApiStandIn, package_of
from ._build import use_case_module
from ._problems import score_of, small_problem

KEY = "om_test_key"
#: The use case the API does not serve: it solves on this machine either way.
LOCAL_USE_CASES = {"school_timetabling"}


@pytest.fixture
def api(monkeypatch):
    """The stand-in, with the package switched onto it."""
    monkeypatch.setattr(_api, "_POLL_FIRST", 0.01)
    with ApiStandIn(KEY) as stand_in:
        optimaru.use_api(KEY, url=stand_in.url)
        yield stand_in


def model_of(slug: str):
    """The small problem of a use case as its pydantic model."""
    package = package_of(slug)
    use_case_module(package)
    model = getattr(importlib.import_module(f"optimaru.{package}.models"), MODEL_USE_CASES[slug])
    problem = small_problem(package)
    return problem if isinstance(problem, model) else model.from_core(problem)


def request_of(slug: str):
    """A small request of a use case the API solves with solve_request()."""
    package = package_of(slug)
    use_case_module(package)
    models = importlib.import_module(f"optimaru.{package}.models")
    if slug == "employee-scheduling":
        from optimaru.employee_scheduling.demo import generate_demo_data

        return generate_demo_data(workers=6, days=4).model_copy(update={"time_limit_seconds": 0.2})
    if slug == "order-picking":
        place = models.LocationModel(column="D", shelving_row=1, side="RIGHT", row=4)
        start = models.LocationModel(column="A", shelving_row=1, side="LEFT", row=0)
        items = [models.ProductModel(id=f"p{i}", name=f"Product {i}", volume=1200, location=place) for i in range(3)]
        return models.OrderPickingRequest(
            time_limit_seconds=0.2,
            trolleys=[models.TrolleyModel(id="1", bucket_count=4, bucket_capacity=48000, location=start)],
            orders=[models.OrderModel(id="order1", items=items)],
        )
    if slug == "pickup-delivery":

        def stop(name):
            return models.StopModel(id=name, location=(37.5, 127.0), max_end_time=10**7, service_duration=60)

        deliveries = [
            models.DeliveryModel(id=f"r{i}", demand=1, pickup=stop(f"r{i}p"), dropoff=stop(f"r{i}d")) for i in range(3)
        ]
        vehicles = [models.VehicleModel(id="v1", capacity=4, home=(37.5, 127.0))]
        size = 1 + 2 * len(deliveries)
        seconds = [[60 * abs(a - b) for b in range(size)] for a in range(size)]
        model_input = models.PdpInput(vehicles=vehicles, deliveries=deliveries, driving_seconds=seconds)
        return models.PdpRequest(time_limit_seconds=0.2, model_input=model_input)
    assert slug == "vehicle-routing"
    visits = [
        models.VisitModel(
            id=f"C{i}", location=(37.50 + i * 0.01, 127.0), demand=2, max_end_time=20 * 3600, service_duration=600
        )
        for i in range(5)
    ]
    vehicles = [models.VehicleModel(id=f"V{i}", capacity=6, depot=(37.5, 127.0)) for i in range(2)]
    return models.RoutePlanRequest(
        time_limit_seconds=0.2, model_input=models.RoutingInput(vehicles=vehicles, visits=visits)
    )


# --- the setting ----------------------------------------------------------------------------


def test_without_a_key_nothing_is_sent():
    assert _api._settings() is None
    knapsack = use_case_module("knapsack")
    solved = knapsack.solve(model_of("knapsack"), time_limit_seconds=0.05)
    assert solved.score_hard == 0


def test_the_environment_variable_alone_moves_a_solve_to_the_api(monkeypatch):
    knapsack = use_case_module("knapsack")
    monkeypatch.setattr(_api, "_POLL_FIRST", 0.01)
    with ApiStandIn(KEY) as stand_in:
        monkeypatch.setenv("OPTIMARU_API_KEY", KEY)
        monkeypatch.setenv("OPTIMARU_API_URL", stand_in.url + "/")
        solved = knapsack.solve(model_of("knapsack"), time_limit_seconds=0.05)
        assert solved.score_hard == 0
        (created,) = stand_in.created()
        assert created.path == "/api/use-cases/knapsack/solve/jobs"
        assert created.headers["x-api-key"] == KEY
        assert created.headers["user-agent"].startswith("optimaru-python/")


def test_the_api_is_at_app_optimaru_com_unless_told_otherwise(monkeypatch):
    monkeypatch.setenv("OPTIMARU_API_KEY", KEY)
    assert _api._settings() == (KEY, "https://app.optimaru.com")
    monkeypatch.setenv("OPTIMARU_API_URL", "https://api.example.com/")
    assert _api._settings() == (KEY, "https://api.example.com")
    optimaru.use_api("om_other", url="https://other.example.com")
    assert _api._settings() == ("om_other", "https://other.example.com")


def test_use_api_without_a_key_reads_the_environment(monkeypatch):
    with pytest.raises(ValueError, match="OPTIMARU_API_KEY"):
        optimaru.use_api()
    monkeypatch.setenv("OPTIMARU_API_KEY", " om_from_env ")
    optimaru.use_api(url="http://localhost:8000")
    assert _api._settings() == ("om_from_env", "http://localhost:8000")
    monkeypatch.delenv("OPTIMARU_API_KEY")
    with pytest.raises(optimaru.ApiError, match="OPTIMARU_API_KEY"):
        _api._settings()


def test_use_local_wins_over_the_environment(monkeypatch):
    monkeypatch.setenv("OPTIMARU_API_KEY", KEY)
    optimaru.use_local()
    assert _api._settings() is None
    optimaru.use_api()
    assert _api._settings() == (KEY, "https://app.optimaru.com")


@pytest.mark.parametrize("key", ["", "   ", 7, b"om_bytes"])
def test_use_api_refuses_a_key_that_is_not_one(key):
    with pytest.raises((TypeError, ValueError)):
        optimaru.use_api(key)
    assert _api._settings() is None


@pytest.mark.parametrize("url", ["http://api.example.com", "ftp://localhost", "app.optimaru.com", "https://"])
def test_the_key_only_travels_over_https_or_to_localhost(monkeypatch, url):
    with pytest.raises(ValueError, match="https"):
        optimaru.use_api(KEY, url=url)
    monkeypatch.setenv("OPTIMARU_API_KEY", KEY)
    monkeypatch.setenv("OPTIMARU_API_URL", url)
    with pytest.raises(ValueError, match="https"):
        _api._settings()


def test_the_settings_are_not_use_cases():
    assert {"use_api", "use_local", "ApiError"} <= set(dir(optimaru))
    assert not {"use_api", "use_local", "ApiError"} & set(optimaru.__all__)


# --- every use case, on the API or declared local -----------------------------------------------


def test_every_use_case_is_on_the_api_or_declared_local():
    served = {package_of(slug) for slug in (*MODEL_USE_CASES, *REQUEST_USE_CASES)}
    assert served | LOCAL_USE_CASES == set(optimaru.__all__)
    assert not served & LOCAL_USE_CASES


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_the_function_that_solves_here_stays_reachable(use_case):
    module = use_case_module(use_case)
    assert callable(module.solve.__wrapped__)
    if use_case not in LOCAL_USE_CASES and package_of(use_case) not in map(package_of, MODEL_USE_CASES):
        assert callable(module.solve_request.__wrapped__)


@pytest.mark.parametrize("slug", MODEL_USE_CASES)
def test_solve_of_a_model_returns_what_it_returns_here(api, slug):
    module = use_case_module(package_of(slug))
    problem = model_of(slug)
    before = problem.model_dump(mode="json")
    # A step limit and a seed make the search the same one on both sides.
    config = module.SolverConfig()
    config.step_count_limit = 200

    solved = module.solve(problem, time_limit_seconds=30, random_seed=3, config=config)

    assert type(solved) is type(problem) and solved is not problem
    assert problem.model_dump(mode="json") == before
    assert score_of(solved)
    (created,) = api.created()
    assert created.path == f"/api/use-cases/{slug}/solve/jobs"
    assert created.body == {
        "problem": before,
        "time_limit_seconds": 30,
        "random_seed": 3,
        "config": {"step_count_limit": 200},
    }
    here = module.solve.__wrapped__(problem, time_limit_seconds=30, random_seed=3, config=config)
    assert solved.model_dump(mode="json") == here.model_dump(mode="json")


@pytest.mark.parametrize("slug", MODEL_USE_CASES)
def test_a_compiled_problem_is_refused_not_solved_here(api, slug):
    module = use_case_module(package_of(slug))
    model_name = MODEL_USE_CASES[slug]
    with pytest.raises(TypeError, match=rf"takes a {model_name}.*{model_name}\.from_core\(problem\).*use_local"):
        module.solve(model_of(slug).to_core(), time_limit_seconds=0.05)
    assert not api.requests


@pytest.mark.parametrize("slug", MODEL_USE_CASES)
def test_solve_request_of_a_model_use_case_goes_the_same_way(api, slug):
    module = use_case_module(package_of(slug))
    models = importlib.import_module(f"optimaru.{package_of(slug)}.models")
    problem = model_of(slug)
    request_type = next(
        value
        for value in vars(models).values()
        if isinstance(value, type) and issubclass(value, _models().SolveRequest) and value is not _models().SolveRequest
    )
    request = request_type(model_input=problem, time_limit_seconds=30, solver_parameters={"step_count_limit": 50})

    solved = module.solve_request(request)

    assert type(solved) is type(problem)
    (created,) = api.created()
    assert created.body == {
        "problem": problem.model_dump(mode="json"),
        "time_limit_seconds": 30.0,
        "random_seed": 0,
        "parallel_solver_count": 1,
        "config": {"step_count_limit": 50},
    }


def _models():
    from optimaru import _models

    return _models


@pytest.mark.parametrize("slug", REQUEST_USE_CASES)
def test_solve_request_returns_the_response_model(api, slug):
    module = use_case_module(package_of(slug))
    models = importlib.import_module(f"optimaru.{package_of(slug)}.models")
    request = request_of(slug)
    before = request.model_dump(mode="json")

    solution = module.solve_request(request)

    assert type(solution) is getattr(models, REQUEST_USE_CASES[slug][1])
    assert request.model_dump(mode="json") == before
    (created,) = api.created()
    assert created.path == f"/api/use-cases/{slug}/solve/jobs"
    assert created.body == {"request": before}
    here = module.solve_request.__wrapped__(request)
    assert set(solution.model_dump(mode="json")) == set(here.model_dump(mode="json"))


@pytest.mark.parametrize("slug", REQUEST_USE_CASES)
def test_solve_of_a_compiled_problem_names_solve_request(api, slug):
    package = package_of(slug)
    module = use_case_module(package)
    with pytest.raises(TypeError, match=r"solve_request\(\).*use_local"):
        module.solve(small_problem(package), time_limit_seconds=0.05)
    assert not api.requests


def test_school_timetabling_solves_here_and_says_so_once(api):
    school = use_case_module("school_timetabling")
    problem = small_problem("school_timetabling")
    with pytest.warns(RuntimeWarning, match="school timetabling is not on the optimaru API"):
        first = school.solve(problem, time_limit_seconds=0.2, random_seed=1)
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        school.solve(problem, time_limit_seconds=0.2, random_seed=1)
    assert first["lessons"]
    assert not api.requests


# --- constraints and config as the API reads them back ------------------------------------------


def test_a_constraint_selection_arrives_as_the_same_picks(api):
    knapsack = use_case_module("knapsack")
    from optimaru.knapsack import constraints as specs

    picks = [
        specs.KnapsackCapacityCost(weight=2, level="hard"),
        specs.SelectedValueCost(exponent=2.0),
        specs.SelectedValueCost(weight=0.5, offset=3, clip=True),
    ]
    solved = knapsack.solve(model_of("knapsack"), time_limit_seconds=0.05, constraints=picks)

    (created,) = api.created()
    assert created.body["constraints"] == [
        {"name": "knapsack_capacity_cost", "weight": 2, "level": "hard", "exponent": 1.0, "offset": 0.0, "clip": True},
        {"name": "selected_value_cost", "exponent": 2.0, "offset": 0.0, "clip": True},
        {"name": "selected_value_cost", "weight": 0.5, "exponent": 1.0, "offset": 3, "clip": True},
    ]
    ((slug, keywords),) = api.calls
    assert slug == "knapsack" and keywords["constraints"] == picks
    assert solved.score_hard == 0


def test_a_constraint_with_domain_parameters_keeps_them(api):
    module = use_case_module("employee_scheduling")
    from optimaru.employee_scheduling import constraints as specs

    request = request_of("employee-scheduling")
    picks = [*specs.get_constraints(), specs.MaxConsecutiveDaysCost(value=3, worker_indices=(0, 2))]
    module.solve_request(request, constraints=picks)

    (created,) = api.created()
    # Employee scheduling carries its selection inside the request, on the API too.
    assert set(created.body) == {"request"}
    sent = created.body["request"]["model_input"]["constraints"]
    assert len(sent) == len(picks)
    assert sent[-1]["name"] == "max_consecutive_days_cost" and sent[-1]["value"] == 3
    assert sent[-1]["worker_indices"] == [0, 2]


def test_a_request_use_case_sends_its_selection_beside_the_request(api):
    module = use_case_module("vehicle_routing")
    from optimaru.vehicle_routing.constraints import TravelTimeCost, get_constraints

    picks = [*get_constraints(), TravelTimeCost(level="medium", offset=7200)]
    module.solve_request(request_of("vehicle-routing"), constraints=picks)

    (created,) = api.created()
    assert set(created.body) == {"request", "constraints"}
    assert created.body["constraints"][-1] == {
        "name": "travel_time_cost",
        "level": "medium",
        "exponent": 1.0,
        "offset": 7200,
        "clip": True,
    }
    ((_, keywords),) = api.calls
    assert keywords["constraints"] == picks


def test_a_pick_added_to_the_request_in_place_is_sent_with_its_name(api):
    module = use_case_module("employee_scheduling")
    from optimaru.employee_scheduling import constraints as specs

    request = request_of("employee-scheduling")
    count = len(request.model_input.constraints)
    request.model_input.constraints.append(specs.MaxConsecutiveDaysCost(value=4))  # past pydantic

    module.solve_request(request)

    (created,) = api.created()
    sent = created.body["request"]["model_input"]["constraints"]
    assert len(sent) == count + 1 and all(item.get("name") for item in sent)
    assert (sent[-1]["name"], sent[-1]["value"]) == ("max_consecutive_days_cost", 4)


def test_free_form_solver_parameters_are_made_plain_or_refused(api):
    module = use_case_module("vehicle_routing")
    request = request_of("vehicle-routing")
    acceptor = next(
        getattr(module.AcceptorType, name)
        for name in dir(module.AcceptorType)
        if name.isupper() and getattr(module.AcceptorType, name) != module.SolverConfig().acceptor_type
    )
    named = request.model_copy(update={"solver_parameters": {"acceptor_type": acceptor, "step_count_limit": 50}})
    module.solve_request(named)
    (created,) = api.created()
    assert created.body["request"]["solver_parameters"] == {"acceptor_type": acceptor.name, "step_count_limit": 50}

    # pydantic would write this limit as [0, null], which the API reads as "leave it as it is".
    endless = request.model_copy(update={"solver_parameters": {"best_score_limit": (0, float("-inf"))}})
    with pytest.raises(ValueError, match="solver_parameters.*non-finite"):
        module.solve_request(endless)
    assert len(api.created()) == 1


def test_a_subclass_of_the_model_comes_back_as_one(api):
    knapsack = use_case_module("knapsack")
    problem = model_of("knapsack")

    class Rucksack(type(problem)):
        pass

    mine = Rucksack.model_validate(problem.model_dump())
    solved = knapsack.solve(mine, time_limit_seconds=0.05)
    assert type(solved) is Rucksack
    assert type(knapsack.solve.__wrapped__(mine, time_limit_seconds=0.05)) is Rucksack


@pytest.mark.parametrize("constraints", [[], ["knapsack_capacity_cost"], [object()]])
def test_a_selection_that_is_not_a_list_of_specs_is_refused(api, constraints):
    knapsack = use_case_module("knapsack")
    with pytest.raises((TypeError, ValueError), match="constraints must"):
        knapsack.solve(model_of("knapsack"), constraints=constraints)
    assert not api.requests


def test_a_config_arrives_as_the_fields_that_were_changed(api):
    module = use_case_module("quadratic_assignment")
    config = module.SolverConfig()
    acceptor = next(
        getattr(module.AcceptorType, name)
        for name in dir(module.AcceptorType)
        if name.isupper() and getattr(module.AcceptorType, name) != config.acceptor_type
    )
    config.acceptor_type = acceptor
    config.simulated_annealing_starting_temperature = (0, 12.5)
    config.late_acceptance_size = 77
    config.step_count_limit = 100
    config.random_seed = 5  # the keyword below overrides it, as it does here

    module.solve(model_of("quadratic-assignment"), time_limit_seconds=30, random_seed=9, config=config)

    (created,) = api.created()
    assert created.body["config"] == {
        "acceptor_type": acceptor.name,
        "simulated_annealing_starting_temperature": [0, 12.5],
        "late_acceptance_size": 77,
        "step_count_limit": 100,
        "random_seed": 5,
    }
    assert created.body["random_seed"] == 9
    ((_, keywords),) = api.calls
    rebuilt = keywords["config"]
    assert rebuilt.acceptor_type == acceptor
    assert rebuilt.simulated_annealing_starting_temperature == (0, 12.5)
    assert (rebuilt.late_acceptance_size, rebuilt.step_count_limit) == (77, 100)


@pytest.mark.parametrize("use_case", ["conference_scheduling", "flight_crew_scheduling", "sports_league_scheduling"])
def test_a_pydantic_solve_config_is_read_as_solve_reads_it(api, use_case):
    module = use_case_module(use_case)
    models = importlib.import_module(f"optimaru.{use_case}.models")
    slug = use_case.replace("_", "-")
    config = models.SolveConfig(time_limit_seconds=20, random_seed=4, step_count_limit=60)

    solved = module.solve(model_of(slug), config=config)

    (created,) = api.created()
    sent = created.body["config"]
    assert (sent["spent_limit_seconds"], sent["random_seed"], sent["step_count_limit"]) == (20, 4, 60)
    here = module.solve.__wrapped__(model_of(slug), config=config)
    assert solved.model_dump(mode="json") == here.model_dump(mode="json")


def test_a_dict_goes_where_solve_takes_one(api):
    module = use_case_module("flight_crew_scheduling")
    problem = model_of("flight-crew-scheduling")
    solved = module.solve(problem.model_dump(mode="json"), time_limit_seconds=0.05)
    assert type(solved) is type(problem)
    (created,) = api.created()
    assert created.body["problem"] == problem.model_dump(mode="json")
    # Elsewhere solve() takes the model or the compiled problem, with the API on as without it.
    with pytest.raises(TypeError, match="takes a KnapsackProblemModel, not a dict"):
        use_case_module("knapsack").solve(model_of("knapsack").model_dump(mode="json"))


@pytest.mark.parametrize("use_case", ["conference_scheduling", "flight_crew_scheduling", "sports_league_scheduling"])
def test_a_first_feasible_limit_in_a_config_travels_as_the_keyword(api, use_case):
    """`models.SolveConfig(stop_at_first_feasible=True)` is a limit of (0, -inf), which JSON cannot carry."""
    module = use_case_module(use_case)
    models = importlib.import_module(f"optimaru.{use_case}.models")
    slug = use_case.replace("_", "-")

    solved = module.solve(model_of(slug), time_limit_seconds=5, config=models.SolveConfig(stop_at_first_feasible=True))

    (created,) = api.created()
    assert created.body["stop_at_first_feasible"] is True
    assert "best_score_limit" not in created.body.get("config", {})
    ((_, keywords),) = api.calls
    assert keywords["stop_at_first_feasible"] is True
    assert score_of(solved)


def test_the_stand_in_refuses_what_the_api_refuses(api):
    """The stand-in is as strict as the API's request model, so a body it takes is one the API takes."""
    knapsack = use_case_module("knapsack")
    problem = _api._model_json(model_of("knapsack"))
    settings = _api._settings()
    for body, where in (
        ({"time_limit_seconds": 0}, "time_limit_seconds"),
        ({"parallel_solver_count": 17}, "parallel_solver_count"),
        ({"config": {"parallel_solver_count": 0}}, "config.parallel_solver_count"),
        ({"full_assert": 1}, "full_assert"),
        ({"constraints": []}, "constraints"),
        ({"constraints": [{"name": "knapsack_capacity_cost", "weight": True}]}, "constraints.0.weight"),
        ({"constraints": [{"name": "knapsack_capacity_cost", "clip": None}]}, "constraints.0.clip"),
        ({"constraints": [{"name": "knapsack_capacity_cost", "hard": True}]}, "constraints.0.hard"),
    ):
        with pytest.raises(optimaru.ApiError, match=f"422 VALIDATION_ERROR: {where}"):
            _api._solve_job(settings, "knapsack", {"problem": problem, **body})
    assert knapsack.solve(model_of("knapsack"), time_limit_seconds=0.05).score_hard == 0


def test_a_time_limit_of_zero_travels_as_the_config_field(api):
    knapsack = use_case_module("knapsack")
    solved = knapsack.solve(model_of("knapsack"), time_limit_seconds=0)
    (created,) = api.created()
    assert "time_limit_seconds" not in created.body
    assert created.body["config"] == {"spent_limit_seconds": 0}
    here = knapsack.solve.__wrapped__(model_of("knapsack"), time_limit_seconds=0)
    assert solved.model_dump(mode="json") == here.model_dump(mode="json")


def test_the_other_solve_keywords_travel_under_their_names(api):
    knapsack = use_case_module("knapsack")
    knapsack.solve(
        model_of("knapsack"),
        time_limit_seconds=0.05,
        parallel_solver_count=2,
        stop_at_first_feasible=True,
        full_assert=True,
    )
    (created,) = api.created()
    assert {name: created.body[name] for name in created.body if name != "problem"} == {
        "time_limit_seconds": 0.05,
        "parallel_solver_count": 2,
        "stop_at_first_feasible": True,
        "full_assert": True,
    }


def test_what_cannot_be_sent_is_refused_before_anything_is_sent(api):
    knapsack = use_case_module("knapsack")
    problem = model_of("knapsack")
    with pytest.raises(ValueError, match="on_new_best is a callback"):
        knapsack.solve(problem, on_new_best=lambda step, score: None)
    config = knapsack.SolverConfig()
    config.on_new_best = lambda step, score: None
    with pytest.raises(ValueError, match="on_new_best is a callback"):
        knapsack.solve(problem, config=config)
    config = knapsack.SolverConfig()
    config.best_score_limit = (-3, float("-inf"))  # not the first feasible solution, which travels as the keyword
    with pytest.raises(ValueError, match="config.best_score_limit.*non-finite.*stop_at_first_feasible=True"):
        knapsack.solve(problem, config=config)
    module = use_case_module("employee_scheduling")
    with pytest.raises(ValueError, match="select cannot be sent"):
        module.solve_request(request_of("employee-scheduling"), select=lambda solutions: solutions[0])
    with pytest.raises(TypeError):
        knapsack.solve(problem, no_such_keyword=1)
    assert not api.requests


# --- the job, and what the API says when it says no ---------------------------------------------


def test_a_solve_waits_for_a_job_that_takes_several_looks(api):
    api.looks_while_running = 3
    knapsack = use_case_module("knapsack")
    solved = knapsack.solve(model_of("knapsack"), time_limit_seconds=0.05)
    assert solved.score_hard == 0
    looks = [r for r in api.requests if r.method == "GET" and not r.path.endswith("/result")]
    assert len(looks) == 4


def test_a_look_that_fails_is_tried_again(api):
    api.fail_looks = 2
    knapsack = use_case_module("knapsack")
    assert knapsack.solve(model_of("knapsack"), time_limit_seconds=0.05).score_hard == 0


def test_a_throttled_look_does_not_cost_the_job(api):
    api.fail_with = (429, {"code": "RATE_LIMITED", "message": "slow down", "detail": None}, {"Retry-After": "0"})
    api.fail_looks = 3
    knapsack = use_case_module("knapsack")
    assert knapsack.solve(model_of("knapsack"), time_limit_seconds=0.05).score_hard == 0
    assert not api.cancelled


def test_the_read_of_the_result_is_tried_again(api):
    api.fail_results = 2
    knapsack = use_case_module("knapsack")
    assert knapsack.solve(model_of("knapsack"), time_limit_seconds=0.05).score_hard == 0
    reads = [r for r in api.requests if r.path.endswith("/result")]
    assert len(reads) == 3


def test_a_job_that_cannot_be_looked_at_is_given_up_and_cancelled(api, monkeypatch):
    monkeypatch.setattr(_api, "_POLL_PATIENCE", 0.05)
    api.fail_looks = 10**6
    knapsack = use_case_module("knapsack")
    with pytest.raises(optimaru.ApiError) as raised:
        knapsack.solve(model_of("knapsack"), time_limit_seconds=0.05)
    assert raised.value.status == 503 and raised.value.code == "QUEUE_UNAVAILABLE"
    assert len(api.cancelled) == 1


def test_a_look_the_api_refuses_is_not_tried_again(api):
    api.fail_with = (404, {"code": "JOB_NOT_FOUND", "message": "no such job", "detail": None}, {})
    api.fail_looks = 10**6
    knapsack = use_case_module("knapsack")
    with pytest.raises(optimaru.ApiError) as raised:
        knapsack.solve(model_of("knapsack"), time_limit_seconds=0.05)
    assert raised.value.code == "JOB_NOT_FOUND"
    looks = [r for r in api.requests if r.method == "GET"]
    assert len(looks) == 1


def test_a_status_the_package_does_not_know_is_a_job_on_its_way(api):
    api.looks_while_running, api.running_as = 2, "preparing"
    knapsack = use_case_module("knapsack")
    assert knapsack.solve(model_of("knapsack"), time_limit_seconds=0.05).score_hard == 0
    assert not api.cancelled


def test_an_interrupted_solve_cancels_its_job(api, monkeypatch):
    api.looks_while_running = 100
    naps = []

    def nap(seconds):
        naps.append(seconds)
        if len(naps) == 3:
            raise KeyboardInterrupt

    monkeypatch.setattr("time.sleep", nap)
    knapsack = use_case_module("knapsack")
    with pytest.raises(KeyboardInterrupt):
        knapsack.solve(model_of("knapsack"), time_limit_seconds=0.05)
    assert len(api.cancelled) == 1
    assert next(iter(api.jobs.values()))["status"] == "cancelled"


def test_the_engine_refusing_the_input_fails_the_job_with_its_words(api):
    knapsack = use_case_module("knapsack")
    from optimaru.knapsack import constraints as specs

    with pytest.raises(optimaru.ApiError, match="no medium level") as raised:
        knapsack.solve(model_of("knapsack"), constraints=[specs.KnapsackCapacityCost(level="medium")])
    assert raised.value.code == "JOB_FAILED" and raised.value.status is None


def test_a_job_cancelled_on_the_api_is_an_error(api):
    api.end_as = ("cancelled", None)
    knapsack = use_case_module("knapsack")
    with pytest.raises(optimaru.ApiError, match="cancelled") as raised:
        knapsack.solve(model_of("knapsack"))
    assert raised.value.code == "JOB_CANCELLED"


def test_a_wrong_key_is_an_api_error(api):
    optimaru.use_api("om_wrong", url=api.url)
    knapsack = use_case_module("knapsack")
    with pytest.raises(optimaru.ApiError, match="401 INVALID_CREDENTIALS: the API key was not accepted") as raised:
        knapsack.solve(model_of("knapsack"))
    assert (raised.value.status, raised.value.code) == (401, "INVALID_CREDENTIALS")


def test_a_plan_limit_says_which_one(api):
    entitlements = {"plan_id": "free", "max_job_seconds": 60, "remaining_seconds": 0}
    api.refuse = (
        402,
        {
            "code": "PLAN_LIMIT_EXCEEDED",
            "message": "over the limit of the plan",
            "detail": {"reason": "quota_exceeded", "entitlements": entitlements},
        },
        {},
    )
    knapsack = use_case_module("knapsack")
    with pytest.raises(
        optimaru.ApiError, match="402 PLAN_LIMIT_EXCEEDED: the plan's included solver time is used up"
    ) as raised:
        knapsack.solve(model_of("knapsack"))
    assert raised.value.detail == {"reason": "quota_exceeded", "entitlements": entitlements}
    assert "(over the limit of the plan)" in str(raised.value)  # the API's own sentence follows


def test_too_many_jobs_at_once_says_when_to_retry(api):
    api.refuse = (
        429,
        {
            "code": "CONCURRENT_JOB_LIMIT",
            "message": "busy",
            "detail": {"reason": "concurrent_jobs", "entitlements": {}},
        },
        {"Retry-After": "7"},
    )
    knapsack = use_case_module("knapsack")
    with pytest.raises(optimaru.ApiError, match="jobs running at once is reached.*retry after 7 s") as raised:
        knapsack.solve(model_of("knapsack"))
    assert (raised.value.status, raised.value.retry_after) == (429, 7.0)
    # Creating a job is not tried again: whether it was created is not known.
    assert len(api.requests) == 1


def test_a_body_the_api_does_not_take_says_where(api):
    detail = [{"type": "less_than_equal", "loc": ["body", "parallel_solver_count"], "msg": "must be between 1 and 16"}]
    api.refuse = (422, {"code": "VALIDATION_ERROR", "message": "invalid", "detail": detail}, {})
    knapsack = use_case_module("knapsack")
    with pytest.raises(
        optimaru.ApiError, match="422 VALIDATION_ERROR: parallel_solver_count: must be between 1 and 16"
    ):
        knapsack.solve(model_of("knapsack"), parallel_solver_count=99)


def test_an_answer_that_is_not_the_apis_own_is_still_an_api_error(api):
    api.refuse = (404, b"<html>not here</html>", {})
    knapsack = use_case_module("knapsack")
    with pytest.raises(optimaru.ApiError, match="404: nothing is served at that address") as raised:
        knapsack.solve(model_of("knapsack"))
    assert raised.value.code is None


def test_a_redirect_is_not_followed_so_the_key_stays_with_the_api(api):
    with ApiStandIn(KEY) as elsewhere:
        api.refuse = (307, {"detail": "moved"}, {"Location": f"{elsewhere.url}/api/use-cases/knapsack/solve/jobs"})
        knapsack = use_case_module("knapsack")
        with pytest.raises(
            optimaru.ApiError, match="307: the address redirects to .*the key is not sent along"
        ) as raised:
            knapsack.solve(model_of("knapsack"))
        assert raised.value.status == 307
        assert not elsewhere.requests


def test_a_job_that_points_off_the_api_is_not_followed(api):
    api.refuse = (
        202,
        {"id": "1", "status": "queued", "status_url": "https://elsewhere.example/job", "result_url": "/result"},
        {},
    )
    knapsack = use_case_module("knapsack")
    with pytest.raises(optimaru.ApiError, match="did not answer with a solve job"):
        knapsack.solve(model_of("knapsack"))
    assert len(api.requests) == 1


def test_an_api_that_cannot_be_reached_is_an_api_error():
    with socket.socket() as taken:
        taken.bind(("127.0.0.1", 0))
        port = taken.getsockname()[1]
    optimaru.use_api(KEY, url=f"http://127.0.0.1:{port}")
    knapsack = use_case_module("knapsack")
    with pytest.raises(optimaru.ApiError, match="could not reach the optimaru API") as raised:
        knapsack.solve(model_of("knapsack"))
    assert raised.value.status is None


def test_a_result_this_version_cannot_read_is_an_api_error(api):
    api.result = {"not": "a knapsack"}
    knapsack = use_case_module("knapsack")
    with pytest.raises(optimaru.ApiError, match="cannot read"):
        knapsack.solve(model_of("knapsack"), time_limit_seconds=0.05)


def test_a_non_finite_number_in_the_problem_is_refused(api):
    module = use_case_module("facility_location")
    problem = model_of("facility-location")
    problem.facilities[0].location.latitude = float("inf")
    with pytest.raises(ValueError, match="non-finite"):
        module.solve(problem)
    assert not api.requests


def test_numpy_values_travel_as_plain_numbers(api):
    numpy = pytest.importorskip("numpy")
    assert _api._plain(numpy.bool_(True), "x") is True
    assert _api._plain(numpy.int64(3), "x") == 3 and type(_api._plain(numpy.int64(3), "x")) is int
    assert _api._plain(numpy.array([1.5, 2.5]), "x") == [1.5, 2.5]
    knapsack = use_case_module("knapsack")
    from optimaru.knapsack import constraints as specs

    picks = [specs.KnapsackCapacityCost(weight=numpy.float64(2.0)), specs.SelectedValueCost(exponent=numpy.float32(2))]
    knapsack.solve(model_of("knapsack"), time_limit_seconds=0.05, random_seed=numpy.int64(4), constraints=picks)
    (created,) = api.created()
    assert created.body["random_seed"] == 4
    assert [(item["name"], item.get("weight"), item["exponent"]) for item in created.body["constraints"]] == [
        ("knapsack_capacity_cost", 2.0, 1.0),
        ("selected_value_cost", None, 2.0),
    ]


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_a_config_left_at_its_defaults_sends_no_field(use_case):
    """Every field of every SolverConfig compares equal to its default, so only a changed one travels."""
    module = use_case_module(use_case)
    assert _api._config_json(module.SolverConfig()) == {}


def test_the_addresses_of_a_job_are_paths_on_the_api(monkeypatch):
    """The API may sit under a path prefix: every address of a solve is that prefix and a path."""
    sent = []

    def send(method, url, key, body=None, *, timeout=None):
        sent.append((method, url))
        if method == "POST":
            job = "/api/use-cases/knapsack/solve/jobs/1"
            return {"id": "1", "status": "succeeded", "status_url": job, "result_url": f"{job}/result"}
        return _api._model_json(model_of("knapsack"))

    monkeypatch.setattr(_api, "_send", send)
    optimaru.use_api(KEY, url="https://host.example/prefix/")
    use_case_module("knapsack").solve(model_of("knapsack"))
    assert sent == [
        ("POST", "https://host.example/prefix/api/use-cases/knapsack/solve/jobs"),
        ("GET", "https://host.example/prefix/api/use-cases/knapsack/solve/jobs/1/result"),
    ]


def test_the_first_solve_sent_to_an_api_is_logged(api, caplog):
    knapsack = use_case_module("knapsack")
    with caplog.at_level("INFO", logger="optimaru"):
        knapsack.solve(model_of("knapsack"), time_limit_seconds=0.05)
        knapsack.solve(model_of("knapsack"), time_limit_seconds=0.05)
    assert [record.getMessage() for record in caplog.records] == [f"solving on the optimaru API at {api.url}"]


def test_the_stand_in_reads_json_only():
    """What arrived at the stand-in went through JSON, so nothing rode along as a Python object."""
    with ApiStandIn(KEY) as stand_in:
        optimaru.use_api(KEY, url=stand_in.url)
        use_case_module("knapsack").solve(model_of("knapsack"), time_limit_seconds=0.05)
        (created,) = stand_in.created()
        assert json.loads(json.dumps(created.body)) == created.body

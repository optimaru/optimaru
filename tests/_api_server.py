"""A stand-in for the optimaru API, for tests/test_api.py.

It speaks the job protocol of the hosted API on a local port (create a job, look at it,
read its result, cancel it) and solves with this checkout the way the API's runtime
does: the body's first field through the use case's pydantic model, the keywords as the
Python call, ``constraints`` as ``dataclasses.replace(<the default spec>, **item)`` and
``config`` set field by field on a ``SolverConfig()``. A test that solves through it
therefore checks what the client sends against what a server reads back, not against a
recording of it.

The two tables are the API's side of the contract, written out here on purpose: the
slugs it serves, the model that reads ``problem`` (the API calls ``solve``) or the
request and response models (it calls ``solve_request``).
"""

import dataclasses
import importlib
import json
import threading
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, NamedTuple

from pydantic import ValidationError

#: Use cases the API solves with ``solve(problem, ...)``: the model of ``problem``.
MODEL_USE_CASES = {
    "assembly-line-balancing": "AssemblyLineBalancingProblemModel",
    "bed-allocation": "BedPlanModel",
    "bin-packing": "BinPackingProblemModel",
    "conference-scheduling": "ConferenceScheduleModel",
    "facility-location": "FacilityLocationProblemModel",
    "flight-crew-scheduling": "FlightCrewScheduleModel",
    "food-packaging": "PackagingScheduleModel",
    "knapsack": "KnapsackProblemModel",
    "maintenance-scheduling": "MaintenanceScheduleModel",
    "max-cut": "MaxCutProblemModel",
    "meeting-scheduling": "MeetingScheduleModel",
    "project-job-scheduling": "ProjectJobScheduleModel",
    "quadratic-assignment": "QuadraticAssignmentProblemModel",
    "semiconductor-scheduling": "SemiconductorScheduleModel",
    "sports-league-scheduling": "LeagueScheduleModel",
    "task-assigning": "TaskAssigningSolutionModel",
    "tournament-scheduling": "TournamentScheduleModel",
}

#: Use cases the API solves with ``solve_request(request, ...)``: the request and the response model.
REQUEST_USE_CASES = {
    "employee-scheduling": ("ScheduleRequest", "ScheduleSolution"),
    "order-picking": ("OrderPickingRequest", "OrderPickingResponse"),
    "pickup-delivery": ("PdpRequest", "PdpSolution"),
    "vehicle-routing": ("RoutePlanRequest", "RouteSolution"),
}

#: The body of a ``solve`` call beside ``problem``, and of a ``solve_request`` call beside ``request``.
_SOLVE_FIELDS = {
    "time_limit_seconds",
    "random_seed",
    "parallel_solver_count",
    "stop_at_first_feasible",
    "full_assert",
    "config",
    "constraints",
    "initial_solution",
}
_REQUEST_FIELDS = {"constraints", "initial_solution"}


def package_of(slug: str) -> str:
    return slug.replace("-", "_")


def _constraints(package: str, items: list[dict[str, Any]]) -> list[Any]:
    """The picks of a body, built as the API builds them: the default spec of the name, with the item's fields."""
    module = importlib.import_module(f"optimaru.{package}.constraints")
    defaults = {
        spec.name: spec
        for spec in vars(module).values()
        if not isinstance(spec, type) and isinstance(spec, module.Constraint) and spec.name
    }
    picks = []
    for item in items:
        fields = {name: value for name, value in item.items() if name not in ("name", "enabled") and value is not None}
        if item["name"] not in defaults:
            raise ValueError(f"unknown metaheuristics constraint: {item['name']}")
        picks.append(dataclasses.replace(defaults[item["name"]], **fields))
    return picks


def _config(package: str, fields: dict[str, Any]) -> Any:
    """The ``config`` of a body on a compiled ``SolverConfig``: a list for a tuple, a name for an enum member."""
    config = importlib.import_module(f"optimaru.{package}._core").SolverConfig()
    for name, value in fields.items():
        if name.startswith("_") or name == "on_new_best" or not hasattr(config, name):
            raise ValueError(f"unknown solver parameter: {name}")
        if value is None:
            continue
        current = getattr(config, name)
        if isinstance(value, list) and isinstance(current, tuple):
            value = tuple(value)
        if isinstance(value, str) and hasattr(type(current), value):
            value = getattr(type(current), value)
        setattr(config, name, value)
    return config


def solve_as_the_api(slug: str, body: dict[str, Any], calls: list) -> dict[str, Any]:
    """Solve `body` as the API's runtime does and return the result's JSON. `calls` gets the keywords used."""
    package = package_of(slug)
    domain = importlib.import_module(f"optimaru.{package}")
    models = importlib.import_module(f"optimaru.{package}.models")
    keywords = {name: body[name] for name in body if name not in ("problem", "request", "initial_solution")}
    if keywords.get("constraints") is not None:
        keywords["constraints"] = _constraints(package, keywords["constraints"])
    if keywords.get("config") is not None:
        keywords["config"] = _config(package, keywords["config"])
    calls.append((slug, keywords))
    # `__wrapped__` is the function that solves here: this process has the API switched on.
    if slug in MODEL_USE_CASES:
        model = getattr(models, MODEL_USE_CASES[slug])
        problem = model.model_validate(body["problem"])
        if body.get("initial_solution") is not None:
            problem = models.with_initial_solution(problem, body["initial_solution"])
        core = problem.to_core()
        domain.solve.__wrapped__(core, **keywords)
        return model.from_core(core).model_dump(mode="json")
    request = getattr(models, REQUEST_USE_CASES[slug][0]).model_validate(body["request"])
    if body.get("initial_solution") is not None:
        request = models.with_initial_solution(request, body["initial_solution"])
    return models.solve_request.__wrapped__(request, **keywords).model_dump(mode="json")


class Received(NamedTuple):
    """One request the stand-in got."""

    method: str
    path: str
    headers: dict[str, str]
    body: Any


class ApiStandIn:
    """The API on ``http://127.0.0.1:<port>``, as a context manager.

    ``key`` is the one key it accepts. A test steers it through the attributes:
    ``looks_while_running`` status answers say ``running_as`` before the job is solved,
    ``refuse`` is ``(status, body, headers)`` answered to the next job instead of creating
    it, ``fail_looks`` status answers and ``fail_results`` result answers are ``fail_with``
    (a 503 unless set), ``html_looks`` status answers are a 200 page that is not JSON,
    ``result`` replaces every job's result, and ``end_as`` ends every
    job in that status without solving it. ``requests`` lists what arrived, ``calls`` the
    keywords each solve ran with, ``cancelled`` the cancelled jobs.
    """

    def __init__(self, key: str = "om_test_key"):
        self.key = key
        self.requests: list[Received] = []
        self.calls: list[tuple[str, dict[str, Any]]] = []
        self.cancelled: list[str] = []
        self.jobs: dict[str, dict[str, Any]] = {}
        self.looks_while_running = 0
        self.running_as = "running"
        self.fail_looks = 0
        self.html_looks = 0
        self.fail_results = 0
        self.fail_with: tuple[int, Any, dict[str, str]] = (503, _error("QUEUE_UNAVAILABLE", "try again"), {})
        self.refuse: tuple[int, Any, dict[str, str]] | None = None
        self.result: Any = None
        self.end_as: tuple[str, str | None] | None = None
        self._server = ThreadingHTTPServer(("127.0.0.1", 0), self._handler())
        self.url = f"http://127.0.0.1:{self._server.server_address[1]}"

    def __enter__(self):
        # A short poll: shutting down waits for one, and every test starts and stops a stand-in.
        threading.Thread(target=self._server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True).start()
        return self

    def __exit__(self, *exc):
        self._server.shutdown()
        self._server.server_close()

    def created(self) -> list[Received]:
        """The requests that asked for a solve job."""
        return [request for request in self.requests if request.method == "POST" and request.path.endswith("/jobs")]

    # --- the job protocol -------------------------------------------------------------------

    def _document(self, job_id: str) -> dict[str, Any]:
        job = self.jobs[job_id]
        base = f"/api/use-cases/{job['slug']}/solve/jobs/{job_id}"
        return {
            "id": job_id,
            "status": job["status"],
            "solve_status": None,
            "error": job.get("error"),
            "status_url": base,
            "result_url": f"{base}/result",
        }

    def _create(self, slug: str, body: Any) -> tuple[int, Any, dict[str, str]]:
        if self.refuse is not None:
            refusal, self.refuse = self.refuse, None
            return refusal
        if slug not in MODEL_USE_CASES and slug not in REQUEST_USE_CASES:
            return 404, _error("NOT_FOUND", "Not Found"), {}  # the framework's answer: no route
        first, fields = ("problem", _SOLVE_FIELDS) if slug in MODEL_USE_CASES else ("request", _REQUEST_FIELDS)
        if slug == "employee-scheduling":
            fields = fields - {"constraints"}  # its selection is request.model_input.constraints
        wrong = [name for name in body if name != first and name not in fields] if isinstance(body, dict) else ["body"]
        if wrong or first not in body:
            detail = [{"type": "extra_forbidden", "loc": ["body", wrong[0] if wrong else first], "msg": "not a field"}]
            return 422, {"code": "VALIDATION_ERROR", "message": "invalid request", "detail": detail}, {}
        refused = _refused(body)
        if refused is not None:
            detail = [{"type": "value_error", "loc": ["body", *refused[0]], "msg": refused[1]}]
            return 422, {"code": "VALIDATION_ERROR", "message": "invalid request", "detail": detail}, {}
        job_id = str(uuid.uuid4())
        self.jobs[job_id] = {"slug": slug, "body": body, "status": "queued", "looks": 0}
        return 202, self._document(job_id), {}

    def _look(self, job_id: str) -> tuple[int, Any, dict[str, str]]:
        if self.html_looks > 0:  # a captive portal's page, with a 200
            self.html_looks -= 1
            return 200, b"<html><body>Sign in to the network</body></html>", {}
        if self.fail_looks > 0:
            self.fail_looks -= 1
            return self.fail_with
        job = self.jobs[job_id]
        job["looks"] += 1
        if job["status"] not in ("succeeded", "failed", "cancelled"):
            if job["looks"] <= self.looks_while_running:
                job["status"] = self.running_as
            elif self.end_as is not None:
                job["status"], job["error"] = self.end_as
            else:
                try:
                    job["result"] = solve_as_the_api(job["slug"], job["body"], self.calls)
                    job["status"] = "succeeded"
                except (ValidationError, ValueError, OverflowError) as error:  # the engine refused the input
                    job["status"], job["error"] = "failed", str(error)
                except Exception as error:  # anything else: the API keeps the kind and a fixed sentence
                    job["status"], job["error"] = (
                        "failed",
                        f"{type(error).__name__}: an unexpected error during the solve",
                    )
        return 200, self._document(job_id), {}

    def _result(self, job_id: str) -> tuple[int, Any, dict[str, str]]:
        if self.fail_results > 0:
            self.fail_results -= 1
            return self.fail_with
        job = self.jobs[job_id]
        if job["status"] != "succeeded":
            return 409, _error("JOB_HAS_NO_RESULT", f"no result yet (status={job['status']})"), {}
        return 200, job["result"] if self.result is None else self.result, {}

    def _cancel(self, job_id: str) -> tuple[int, Any, dict[str, str]]:
        job = self.jobs[job_id]
        if job["status"] not in ("succeeded", "failed", "cancelled"):
            job["status"] = "cancelled"
            self.cancelled.append(job_id)
        return 200, self._document(job_id), {}

    def _answer(self, method: str, path: str, headers: dict[str, str], body: Any) -> tuple[int, Any, dict[str, str]]:
        if headers.get("x-api-key") != self.key:
            return 401, _error("INVALID_CREDENTIALS", "not a valid API key"), {"WWW-Authenticate": "Bearer"}
        parts = path.strip("/").split("/")
        if parts[:2] != ["api", "use-cases"] or parts[3:5] != ["solve", "jobs"]:
            return 404, _error("NOT_FOUND", "Not Found"), {}
        slug, rest = parts[2], parts[5:]
        if method == "POST" and not rest:
            return self._create(slug, body)
        if rest and rest[0] in self.jobs and self.jobs[rest[0]]["slug"] == slug:
            if method == "GET" and len(rest) == 1:
                return self._look(rest[0])
            if method == "GET" and rest[1:] == ["result"]:
                return self._result(rest[0])
            if method == "POST" and rest[1:] == ["cancel"]:
                return self._cancel(rest[0])
        return 404, _error("JOB_NOT_FOUND", "no such job"), {}

    def _handler(self):
        stand_in = self

        class Handler(BaseHTTPRequestHandler):
            def _serve(self):
                length = int(self.headers.get("Content-Length") or 0)
                body = json.loads(self.rfile.read(length)) if length else None
                headers = {name.lower(): value for name, value in self.headers.items()}
                stand_in.requests.append(Received(self.command, self.path, headers, body))
                status, answer, extra = stand_in._answer(self.command, self.path, headers, body)
                data = answer if isinstance(answer, bytes) else json.dumps(answer).encode()
                self.send_response(status)
                self.send_header("Content-Type", "text/html" if isinstance(answer, bytes) else "application/json")
                self.send_header("Content-Length", str(len(data)))
                for name, value in extra.items():
                    self.send_header(name, value)
                self.end_headers()
                self.wfile.write(data)

            do_GET = do_POST = _serve

            def log_message(self, *args):  # quiet
                pass

        return Handler


def _number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _refused(body: dict[str, Any]) -> tuple[list[Any], str] | None:
    """What the API's request model refuses before a job exists, as (where, why); None when it takes the body.

    Worded and placed as the API reports it (checked against optimaru/server): a field's own
    rule under the field, a rule over several fields under ``body`` as a "Value error". Its
    rules beside the problem: a time limit above 0 and at most 1e9 s
    (``config.spent_limit_seconds`` from 0), 1 to 16 parallel starts (in ``config`` too),
    and in a request the same for its time limit and its starts, each given under one name.
    For a pick: a name, a weight that is a number or null (0 or more), a level by its name,
    an exponent above 0, a number as offset and a boolean as clip, the last two never null.
    Its booleans and integers beside the picks are pydantic's lax ones, so they are not
    checked here.
    """
    longest = 1e9
    limit = body.get("time_limit_seconds")
    if limit is not None and not (_number(limit) and limit > 0):
        return ["time_limit_seconds"], "Input should be greater than 0"
    if limit is not None and limit > longest:
        return ["time_limit_seconds"], "Input should be less than or equal to 1000000000"
    config = body.get("config")
    if config is not None and not isinstance(config, dict):
        return ["config"], "Input should be a valid dictionary"
    request = body.get("request") if isinstance(body.get("request"), dict) else {}
    for new, old in (("time_limit_seconds", "timelimit"), ("parallel_solver_count", "num_workers")):
        if new in request and old in request:
            return [], f"Value error, request.{new} and request.{old} are the same field: give one"
    settings = request.get("settings") if isinstance(request.get("settings"), dict) else {}
    for name, count in (
        ("parallel_solver_count", body.get("parallel_solver_count")),
        ("config.parallel_solver_count", (config or {}).get("parallel_solver_count")),
        ("request.parallel_solver_count", request.get("parallel_solver_count", request.get("num_workers"))),
        ("request.settings.multi_start", settings.get("multi_start")),
    ):
        if count is not None and not (isinstance(count, int) and not isinstance(count, bool) and 1 <= count <= 16):
            return [], f"Value error, {name} must be between 1 and 16"
    for name, seconds in (
        ("config.spent_limit_seconds", (config or {}).get("spent_limit_seconds")),
        ("request.time_limit_seconds", request.get("time_limit_seconds", request.get("timelimit"))),
    ):
        if seconds is not None and not (_number(seconds) and 0 <= seconds <= longest):
            return [], f"Value error, {name} must be between 0 and 1e9 seconds"
    picks = body.get("constraints")
    if picks is None:
        return None
    if not isinstance(picks, list) or not any(isinstance(pick, dict) and pick.get("enabled", True) for pick in picks):
        return ["constraints"], "Value error, constraints must select at least one enabled constraint"
    for position, pick in enumerate(picks):
        where = ["constraints", position]
        if not isinstance(pick, dict) or not isinstance(pick.get("name"), str) or not pick["name"]:
            return [*where, "name"], "Field required"
        if "hard" in pick:
            return where, "Value error, `hard` was replaced by `level`"
        weight = pick.get("weight")
        if weight is not None and not _number(weight):
            return [*where, "weight"], "Input should be a valid number"
        if weight is not None and weight < 0:
            return [*where, "weight"], "Input should be greater than or equal to 0"
        if pick.get("level") not in (None, "hard", "medium", "soft"):
            return [*where, "level"], "Input should be 'hard', 'medium' or 'soft'"
        exponent = pick.get("exponent")
        if exponent is not None and not (_number(exponent) and exponent > 0):
            return [*where, "exponent"], "Input should be greater than 0"
        if "offset" in pick and not _number(pick["offset"]):
            return [*where, "offset"], "Input should be a valid number"
        if "clip" in pick and not isinstance(pick["clip"], bool):
            return [*where, "clip"], "Input should be a valid boolean"
    return None


def _error(code: str, message: str, detail: Any = None) -> dict[str, Any]:
    """The API's error body."""
    return {"code": code, "message": message, "detail": message if detail is None else detail}

"""Solving on the optimaru API instead of on this machine.

This package solves where it is installed. The optimaru API is the same engine run as a
hosted service (optimaru Engine Enterprise), and the body of its solve request is the
Python call: the first argument and the keywords of ``solve()`` or ``solve_request()``,
as JSON. So a program that already uses this package moves onto the API with one
setting, an API key, given in the environment or in code::

    export OPTIMARU_API_KEY=om_live_...

    import optimaru
    optimaru.use_api("om_live_...")

With a key, ``optimaru.<use case>.solve(model, ...)`` and ``solve_request(request, ...)``
send the problem to the API, wait for the solve job and return what they return on this
machine: a new solved model, or the response model. Without one nothing changes.

What a use case's ``__init__.py`` and ``models.py`` do with this module::

    from .. import _api

    @_api.model_solve("knapsack", KnapsackProblemModel)
    def solve(problem, *, time_limit_seconds=None, ..., config=None, constraints=None): ...

    @_api.request_solve("vehicle-routing", "RouteSolution")
    def solve_request(request, *, constraints=None): ...

`model_solve` is for a ``solve()`` that takes the use case's pydantic model (the API calls
``solve``), `request_solve` for a ``solve_request()`` that the API calls itself,
`compiled_solve` for a ``solve()`` that takes compiled objects only, and `local_solve`
for a use case the API does not serve. The decorated function is unchanged while no key
is set, and ``__wrapped__`` is the function that always solves here.

Only the standard library and pydantic are used, and both are imported when a solve is
sent, so ``import optimaru`` stays as light as it was.
"""

import functools
import os
from collections.abc import Callable
from typing import Any, ParamSpec, TypeVar

__all__ = [
    "API_KEY_VARIABLE",
    "API_URL_VARIABLE",
    "DEFAULT_API_URL",
    "ApiError",
    "compiled_solve",
    "local_solve",
    "model_solve",
    "request_solve",
    "use_api",
    "use_local",
]

#: The environment variable that holds the API key. Set, it turns the API on.
API_KEY_VARIABLE = "OPTIMARU_API_KEY"
#: The environment variable that names another API than `DEFAULT_API_URL`.
API_URL_VARIABLE = "OPTIMARU_API_URL"
#: Where the optimaru API is, without a trailing slash.
DEFAULT_API_URL = "https://app.optimaru.com"

_P = ParamSpec("_P")
_R = TypeVar("_R")

# What the program chose in code: (on, key, url). `on` None = it did not choose, and the environment
# decides: the API when `API_KEY_VARIABLE` is set, this machine otherwise. One tuple, replaced as a
# whole, so that a solve in another thread never pairs one call's key with another's URL.
_state: tuple[bool | None, str | None, str | None] = (None, None, None)

#: Seconds one HTTP request may take. A solve is a job that is polled, so no request waits for the search.
_REQUEST_TIMEOUT = 60.0
#: Seconds the request that cancels a job may take: an interrupted solve should not hang on it.
_CANCEL_TIMEOUT = 5.0
#: Seconds between two looks at a job: the first, and the longest the wait grows to.
_POLL_FIRST, _POLL_LONGEST = 0.2, 2.0
#: Seconds a look at a job, or the read of its result, keeps being tried while it fails for a reason
#: that passes (the network, a 5xx, a 408, a 429): a job that runs is not given up for a bad moment.
_POLL_PATIENCE = 60.0
#: The longest a ``Retry-After`` is waited for between two tries, in seconds.
_RETRY_AFTER_LONGEST = 30.0
#: Seconds past twice its time limit a job may take to finish, for the queue and the overhead. A job
#: the API still reports as unfinished then is given up and cancelled (`_wait_limit`).
_WAIT_MARGIN = 600.0
#: The time limit of a solve that names none: the engine's default.
_DEFAULT_TIME_LIMIT = 30.0
#: The statuses a job ends in. Any other one, known or not, is a job still on its way.
_FINAL_STATUSES = ("succeeded", "failed", "cancelled")
#: The keywords every ``solve()`` has that the API takes under the same name.
_SOLVE_KEYWORDS = (
    "time_limit_seconds",
    "random_seed",
    "parallel_solver_count",
    "stop_at_first_feasible",
    "full_assert",
)


class ApiError(RuntimeError):
    """The optimaru API refused a solve, or failed at it.

    ``status`` is the HTTP status of the API's error response, or None for any other error:
    the API could not be reached or answered with something other than a solve job or a
    result this version can read, or a job that was accepted then failed, was cancelled or
    did not finish in time. ``code`` is the API's error code, such as
    ``"INVALID_CREDENTIALS"``, ``"PLAN_LIMIT_EXCEEDED"`` or ``"VALIDATION_ERROR"``;
    ``"JOB_FAILED"``, ``"JOB_CANCELLED"`` and ``"JOB_TIMEOUT"`` are a job that ended without
    a result. ``detail`` is what the API sent with the error: for a plan limit, a dict with
    the ``reason`` and the plan's ``entitlements``. ``retry_after`` is the
    number of seconds the API asked to wait before trying again (its ``Retry-After``),
    or None.
    """

    def __init__(
        self,
        message: str,
        *,
        status: int | None = None,
        code: str | None = None,
        detail: Any = None,
        retry_after: float | None = None,
    ):
        super().__init__(message)
        self.status = status
        self.code = code
        self.detail = detail
        self.retry_after = retry_after


def use_api(key: str | None = None, *, url: str | None = None) -> None:
    """Solve on the optimaru API from now on, in this process.

    ``key`` is the API key (it starts with ``om_``); None reads the environment variable
    ``OPTIMARU_API_KEY`` at each solve. ``url`` is the API to call when it is not the
    default one, ``https://app.optimaru.com``; None reads ``OPTIMARU_API_URL``, then the
    default. It must be an https URL (http is accepted for localhost).

    Setting ``OPTIMARU_API_KEY`` alone does the same without a line of code; this call is
    for a key that comes from somewhere else, and it wins over the environment.
    `use_local` turns the API off again.

    With the API on, ``solve(model, ...)`` and ``solve_request(request, ...)`` send the
    problem and return what they return on this machine. The settings travel as what was
    given: a keyword or a ``SolverConfig`` field left at its default takes the default of
    the engine the API runs; a request carries all its own fields. A solve waits for its
    job, from when the job is created, for at most twice its time limit (times its parallel
    starts, for a request) and ten minutes more, then cancels it. The first solve
    sent to an API is logged at INFO on the ``optimaru`` logger. What cannot be sent is
    refused instead of being solved here without notice: a compiled problem (pass its
    model), a callback (``on_new_best``, ``select``) and a non-finite number. School
    timetabling keeps solving on this machine (the API takes it in a request of its own),
    and so does the compiled ``Solver`` class used directly: the setting moves ``solve``
    and ``solve_request``. The setting applies to this process: a worker process that
    starts fresh needs its own call, or the environment variable.

    Raises ValueError for an empty key, a missing one (no ``key`` and no
    ``OPTIMARU_API_KEY``) and a ``url`` that is not accepted.
    """
    global _state
    if key is not None:
        if not isinstance(key, str):
            raise TypeError(f"key must be a string, not {type(key).__name__}")
        key = _checked_key(key, "key")
    elif not _environment(API_KEY_VARIABLE):
        raise ValueError(f"no API key: pass one, or set {API_KEY_VARIABLE}")
    if url is not None:
        if not isinstance(url, str):
            raise TypeError(f"url must be a string, not {type(url).__name__}")
        url = _checked_url(url)
    _state = (True, key, url)


def use_local() -> None:
    """Solve on this machine from now on, in this process, whatever ``OPTIMARU_API_KEY`` says."""
    global _state
    _state = (False, None, None)


def _environment(name: str) -> str:
    return os.environ.get(name, "").strip()


def _checked_key(key: str, where: str) -> str:
    """`key` without surrounding blanks. Refuses one that is empty or holds a blank or a non-ASCII character.

    Such a key cannot go into an HTTP header as it is, and the error says so without showing it.
    """
    import re

    key = key.strip()
    if not key:
        raise ValueError(f"{where} must not be empty")
    if not re.fullmatch(r"[!-~]+", key):
        raise ValueError(f"{where} must be printable ASCII without spaces or line breaks; check how it was copied")
    return key


def _checked_url(url: str) -> str:
    """`url` without its trailing slash. Refuses what would send the key in the clear, or another secret along."""
    from urllib.parse import urlsplit

    url = url.strip().rstrip("/")
    try:
        parts = urlsplit(url)
        hostname = parts.hostname
    except ValueError:
        parts, hostname = None, None
    if parts is not None and (parts.username or parts.password):
        # Not repeated in the error: the password would be.
        raise ValueError("the API URL must not carry a user name or password")
    local = hostname in ("localhost", "127.0.0.1", "::1")
    if parts is None or parts.scheme not in ("https", "http") or not hostname or (parts.scheme == "http" and not local):
        raise ValueError(f"the API URL must be an https URL (http for localhost only), not {url!r}")
    if parts.query or parts.fragment:
        raise ValueError(f"the API URL must not have a query or a fragment, not {url!r}")
    # The scheme and host in lower case: `_send` tells plain http by its scheme.
    return parts._replace(netloc=parts.netloc.lower()).geturl()  # urlsplit lowered the scheme


def _settings() -> tuple[str, str] | None:
    """The key and the URL of the API to solve on, or None to solve on this machine."""
    chosen, key, url = _state
    if chosen is False:
        return None
    if key is None:
        key = _environment(API_KEY_VARIABLE)
        if not key:
            if chosen:
                raise ApiError(f"use_api() was called without a key and {API_KEY_VARIABLE} is no longer set")
            return None
        key = _checked_key(key, API_KEY_VARIABLE)
    return key, url or _checked_url(_environment(API_URL_VARIABLE) or DEFAULT_API_URL)


def _api_on() -> bool:
    """Whether solves go to the API, without checking the key or the URL.

    For a decorator that sends nothing: a setting it would never use must not make it fail.
    """
    chosen = _state[0]
    if chosen is not None:
        return chosen
    return bool(_environment(API_KEY_VARIABLE))


# ---------------------------------------------------------------------------
# The Python call as the JSON the API takes
# ---------------------------------------------------------------------------


def _plain(value: Any, where: str) -> Any:
    """`value` as JSON can carry it and the API reads it back: `where` names it in an error.

    A member of a compiled enum goes by its name and a tuple as a list, which is how the
    API sets a ``SolverConfig`` field and a constraint parameter from JSON.
    """
    import decimal
    import enum
    import math
    import numbers

    if type(value).__module__ == "numpy" and callable(getattr(value, "tolist", None)):
        value = value.tolist()  # a numpy scalar or array, without importing numpy
    if value is None or (isinstance(value, (bool, str)) and not isinstance(value, enum.Enum)):
        return value
    if isinstance(value, enum.Enum):
        return _plain(value.value, where)
    if isinstance(value, decimal.Decimal):
        value = float(value)  # what the compiled setters read it as
    if isinstance(value, numbers.Integral):
        return int(value)
    if isinstance(value, numbers.Real):
        number = float(value)
        if not math.isfinite(number):
            raise ValueError(f"{where} is {value!r}: a non-finite number cannot be sent to the optimaru API")
        return number
    if isinstance(value, dict):
        return {str(key): _plain(item, where) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_plain(item, where) for item in value]
    if isinstance(value, (set, frozenset)):
        items = [_plain(item, where) for item in value]
        try:
            return sorted(items)
        except TypeError:  # mixed types: an order of its own, the same on every call
            return sorted(items, key=repr)
    if callable(getattr(value, "model_dump", None)):
        return value.model_dump(mode="json")
    name = getattr(value, "name", None)
    if isinstance(name, str) and hasattr(type(value), name):  # a compiled enum: AcceptorType.LATE_ACCEPTANCE
        return name
    raise TypeError(f"{where} is a {type(value).__name__}, which cannot be sent to the optimaru API")


def _model_json(model: Any) -> dict[str, Any]:
    """A problem model or a request as the JSON it is sent as.

    A request's free-form fields (``solver_parameters``, and the ``portfolio`` of employee
    scheduling's ``settings``) are made plain first. pydantic writes a non-finite number
    in such a field as null, which the API reads as "leave the field as it is", and cannot
    write a compiled enum member at all; `_plain` refuses the one and names the other. A
    non-finite number in a typed field stays one and is refused when the body is encoded.
    """
    from pydantic_core import PydanticSerializationError

    update: dict[str, Any] = {}
    parameters = getattr(model, "solver_parameters", None)
    if isinstance(parameters, dict) and parameters:
        update["solver_parameters"] = _plain(parameters, "solver_parameters")
    settings = getattr(model, "settings", None)
    portfolio = getattr(settings, "portfolio", None)
    if isinstance(portfolio, list) and portfolio:
        update["settings"] = settings.model_copy(update={"portfolio": _plain(portfolio, "settings.portfolio")})
    if update:
        model = model.model_copy(update=update)
    try:
        return model.model_dump(mode="json")
    except PydanticSerializationError as error:
        raise TypeError(
            f"the {type(model).__name__} holds a value that cannot be sent to the optimaru API: {error}"
        ) from None


def _constraints_json(constraints: Any) -> list[dict[str, Any]]:
    """A ``constraints=`` selection as the API's list: one object per pick, in order.

    ``{"name": ..., <knob>: ..., <parameter>: ...}``, as the API builds each pick back with
    ``dataclasses.replace(<the default spec of that name>, **the rest)``. A None knob or
    parameter is left out: on both sides it is the default. That holds as long as a spec
    field whose value may be None also defaults to None, which every spec does today.
    """
    import dataclasses

    picks = list(constraints)
    if not picks:
        raise ValueError("constraints must not be empty (pass None for the defaults)")
    items = []
    for pick in picks:
        name = getattr(pick, "name", None)
        if isinstance(pick, type) or not dataclasses.is_dataclass(pick) or not isinstance(name, str) or not name:
            raise TypeError(f"constraints must be constraint specs to be sent to the optimaru API, not {pick!r}")
        item: dict[str, Any] = {"name": name}
        for field in dataclasses.fields(pick):
            value = getattr(pick, field.name)
            if value is not None:
                item[field.name] = _plain(value, f"constraint {name!r}: {field.name}")
        items.append(item)
    return items


def _config_json(config: Any) -> dict[str, Any]:
    """A compiled ``SolverConfig`` as the API's ``config``: the fields that differ from the defaults.

    The API sets each one by name on a ``SolverConfig()`` of its own, so the defaults need
    not travel, and the ones that JSON cannot carry (``best_score_limit`` is infinite by
    default) stay at home unless they were changed.
    """
    if getattr(config, "on_new_best", None) is not None:
        raise ValueError("on_new_best is a callback, which cannot be sent to the optimaru API")
    defaults = type(config)()
    fields = {}
    # Every public attribute of a SolverConfig that is not a method is a field the API sets by
    # name, and compares by value: tests/test_api.py checks that a default config sends nothing.
    for name in dir(config):
        if name.startswith("_") or name == "on_new_best":
            continue
        value = getattr(config, name)
        if callable(value) or value == getattr(defaults, name):
            continue
        try:
            fields[name] = _plain(value, f"config.{name}")
        except ValueError as error:
            if name == "best_score_limit":
                raise ValueError(
                    f"{error} (to stop at the first feasible solution, pass stop_at_first_feasible=True)"
                ) from None
            raise
    return fields


def _solve_body(keywords: dict[str, Any]) -> dict[str, Any]:
    """The keywords of a ``solve()`` call as the top-level fields of the API's body.

    A keyword left at its default is left out, as the API's default is the same one.
    ``config`` travels as the fields that were changed, and ``constraints`` as a list.
    """
    keywords = dict(keywords)
    if keywords.pop("on_new_best", None) is not None:
        raise ValueError("on_new_best is a callback, which cannot be sent to the optimaru API")
    config = keywords.pop("config", None)
    constraints = keywords.pop("constraints", None)
    body: dict[str, Any] = {}
    if callable(getattr(config, "to_core", None)):
        config = config.to_core()  # a models.SolveConfig, where a use case's solve() takes one
    limit = getattr(config, "best_score_limit", None)
    if (
        isinstance(limit, tuple)
        and limit[:1] == (0,)
        and limit[1:]
        and all(level == float("-inf") for level in limit[1:])
    ):
        # Hard 0 and no floor below is what stop_at_first_feasible sets on a float-scored use case,
        # and JSON has no infinity: the limit travels as the keyword, which the API turns into the
        # same limit. (The integer-scored ones use -(2**63), which is finite and travels as it is.)
        # A compiled SolverConfig supports copy.copy (bind_config_copy in the engine's bindings).
        import copy

        config = copy.copy(config)
        config.best_score_limit = type(config)().best_score_limit
        keywords["stop_at_first_feasible"] = True
    fields = {} if config is None else _config_json(config)
    for name in _SOLVE_KEYWORDS:
        value = keywords.pop(name, None)
        # By identity: a seed of 0 travels. False is the default of the two flags, which only turn on.
        if value is None or value is False:
            continue
        if name == "time_limit_seconds" and value == 0:
            # The API takes a budget of 0 (the construction, or the warm start, as it is) only as
            # the config field, like a search that is switched off in config.
            fields["spent_limit_seconds"] = 0
            continue
        body[name] = _plain(value, name)
    for name, value in keywords.items():
        if value is not None and value is not False:
            raise ValueError(f"{name} cannot be sent to the optimaru API")
    if fields:
        body["config"] = fields
    if constraints is not None:
        body["constraints"] = _constraints_json(constraints)
    return body


# ---------------------------------------------------------------------------
# HTTP: one solve is a job that is created, polled and read
# ---------------------------------------------------------------------------


#: What an error means, by the plan limit's reason, the API's error code, or the HTTP status when the
#: answer is not the API's own (a proxy in between).
_ERROR_HINTS: dict[Any, str] = {
    "quota_exceeded": "the plan's included solver time is used up",
    "payment_overdue": "an invoice is overdue",
    "job_time_limit": "the time limit is longer than the plan allows for one job",
    "concurrent_jobs": "the plan's limit of jobs running at once is reached",
    "AUTH_REQUIRED": "the API needs an API key",
    "INVALID_CREDENTIALS": "the API key was not accepted",
    "FORBIDDEN": "the API key may not run solve jobs",
    "TIME_LIMIT_TOO_LONG": "the time limit is longer than one job may run on this API",
    401: "the API key was not accepted",
    403: "the request was refused",
    404: "nothing is served at that address: check the API URL",
    413: "the problem is too large for one request",
}


def _seconds_until(retry_after: str) -> float | None:
    """A ``Retry-After`` as seconds from now: a number of seconds, or an HTTP date. None when it is neither."""
    import email.utils
    import time

    try:
        return max(0.0, float(retry_after))
    except ValueError:
        pass
    try:
        when = email.utils.parsedate_to_datetime(retry_after)
    except (TypeError, ValueError):
        return None
    return max(0.0, when.timestamp() - time.time())


@functools.cache
def _version() -> str:
    from importlib.metadata import PackageNotFoundError, version

    try:
        return version("optimaru")
    except PackageNotFoundError:  # running from a source tree
        return "0.0.0"


def _error_of(status: int, text: str, headers: Any) -> ApiError:
    """The API's error response as an `ApiError`. The body is ``{"code", "message", "detail"}``."""
    import json

    code, message, detail = None, "", None
    try:
        body = json.loads(text)
    except (ValueError, RecursionError):
        body = None  # not the API's own answer: a proxy's page, an empty body
    if isinstance(body, dict):
        code = body.get("code") if isinstance(body.get("code"), str) else None
        message = body.get("message") if isinstance(body.get("message"), str) else ""
        detail = body.get("detail")
    reason = detail.get("reason") if isinstance(detail, dict) else None
    # The hints by status are for an answer that is not the API's own: one with a code says what it means.
    said = _ERROR_HINTS.get(reason) or _ERROR_HINTS.get(code) or (None if code else _ERROR_HINTS.get(status))
    if code == "VALIDATION_ERROR" and isinstance(detail, list) and detail and isinstance(detail[0], dict):
        # pydantic's list of errors: say where the first one is.
        loc, msg = detail[0].get("loc"), detail[0].get("msg")
        parts = loc if isinstance(loc, list) else []
        where = ".".join(str(part) for part in parts if part != "body")
        said = f"{where}: {msg if isinstance(msg, str) else ''}".strip(": ") or said
    if 300 <= status < 400:
        target = headers.get("Location") if headers is not None else None
        where = f" to {target}" if target else ""
        said = f"the address redirects{where}; the key is not sent along, so set the API URL to where it points"
    answer = f"the optimaru API answered {status}" + (f" {code}" if code else "")
    if said or message:
        answer += f": {said or message}"
    if said and message:
        answer += f" ({message})"  # the API's own sentence
    retry = headers.get("Retry-After") if headers is not None else None
    retry_after = _seconds_until(retry) if retry else None
    if retry_after is not None:
        answer += f"; retry after {retry_after:g} s"
    return ApiError(answer, status=status, code=code, detail=detail, retry_after=retry_after)


@functools.cache
def _opener(direct: bool) -> Any:
    """What every request is sent with: it follows no redirect, and with `direct` it uses no proxy.

    urllib would send the request's headers, the key among them, to wherever a redirect
    points. The API does not redirect, so one is an error, and it names the address.
    A plain-http request (to localhost only) goes direct: through a proxy from the
    environment, its key and problem would cross the network in the clear. An https one
    may use a proxy, which only tunnels the encrypted connection.
    """
    import urllib.request

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *args: Any, **keywords: Any) -> None:
            return None

    handlers: list[Any] = [NoRedirect]
    if direct:
        handlers.append(urllib.request.ProxyHandler({}))
    return urllib.request.build_opener(*handlers)


def _send(
    method: str, url: str, key: str, body: dict[str, Any] | None = None, *, timeout: float = _REQUEST_TIMEOUT
) -> Any:
    """One request to the API, and its JSON. Raises `ApiError` for an error response and when the API cannot be reached."""
    import http.client
    import json
    import urllib.error
    import urllib.request

    headers = {"X-API-Key": key, "Accept": "application/json", "User-Agent": f"optimaru-python/{_version()}"}
    data = None
    if body is not None:
        try:
            data = json.dumps(body, allow_nan=False).encode()
        except ValueError as error:
            # allow_nan=False refuses a non-finite number; a circular reference is the other case.
            why = "it holds a non-finite number (inf or nan)" if "float" in str(error) else str(error)
            raise ValueError(f"the request cannot be sent to the optimaru API as JSON: {why}") from error
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        # In any case: _checked_url lowers the scheme, and this check does not rely on it.
        with _opener(url[:7].lower() == "http://").open(request, timeout=timeout) as response:
            text = response.read().decode(errors="replace")
    except urllib.error.HTTPError as error:
        with error:
            raise _error_of(error.code, error.read().decode(errors="replace"), error.headers) from None
    except (OSError, http.client.HTTPException) as error:  # URLError is an OSError
        reason = getattr(error, "reason", None) or error
        raise ApiError(f"could not reach the optimaru API at {url}: {reason}") from error
    try:
        return json.loads(text)
    except (ValueError, RecursionError) as error:
        raise ApiError(f"the optimaru API at {url} did not answer with JSON") from error


def _job_of(answer: Any, url: str) -> dict[str, Any]:
    # The two addresses are paths on the API: anything else is not followed, as the key goes with every request.
    if (
        not isinstance(answer, dict)
        or not isinstance(answer.get("status"), str)
        or not all(
            isinstance(answer.get(name), str) and answer[name].startswith("/") for name in ("status_url", "result_url")
        )
    ):
        raise ApiError(f"the optimaru API at {url} did not answer with a solve job")
    return answer


def _patiently(method: str, url: str, key: str, patience: float = _POLL_PATIENCE) -> Any:
    """`_send` for a job that exists: a failure that passes is tried again, for `patience` seconds.

    The network, a 5xx, a 408 and a 429 say nothing about the job, which runs on and is
    paid for; giving it up for one of them would throw the solve away. What the API says
    about the request itself (a 401, a 404) is raised at once.
    """
    import time

    started = time.monotonic()
    wait = _POLL_FIRST
    while True:
        try:
            return _send(method, url, key)
        except ApiError as error:
            passes = error.status is None or error.status >= 500 or error.status in (408, 429)
            if not passes or time.monotonic() - started > patience:
                raise
            time.sleep(max(wait, min(error.retry_after or 0.0, _RETRY_AFTER_LONGEST)))
            wait = min(wait * 1.5, _POLL_LONGEST)


#: What has been said once in this process: the APIs solves were sent to, and ``local:<use case>``
#: for a use case that solved here with the API on.
_said: set[str] = set()


def _say_once(base: str) -> None:
    """Log, once per API, that solves go there: the problem leaves the machine, which a log should show."""
    if base not in _said:
        import logging

        _said.add(base)
        logging.getLogger("optimaru").info("solving on the optimaru API at %s", base)


def _wait_limit(body: dict[str, Any]) -> float:
    """Seconds to wait for the job of `body` before giving it up: twice its time limit, and a margin.

    The time limit is the one the call asks for (the engine's default when it names none),
    and for a request, times its parallel starts, which can run one after another.
    """

    def seconds(*values: Any) -> float:
        given = next((value for value in values if isinstance(value, (int, float)) and value > 0), None)
        return float(given) if given is not None else _DEFAULT_TIME_LIMIT

    if "problem" in body:
        limit = seconds(body.get("time_limit_seconds"), (body.get("config") or {}).get("spent_limit_seconds"))
    else:
        request = body.get("request") or {}
        settings = request.get("settings") or {}
        starts = max(
            int(request.get("parallel_solver_count") or request.get("num_workers") or 1),
            int(settings.get("multi_start") or 1) if isinstance(settings, dict) else 1,
        )
        limit = seconds(request.get("time_limit_seconds"), request.get("timelimit")) * starts
    return 2 * limit + _WAIT_MARGIN


def _solve_job(settings: tuple[str, str], slug: str, body: dict[str, Any]) -> Any:
    """Create the solve job of `body`, wait for it and return its result: the solved model's JSON."""
    import time

    key, base = settings
    _say_once(base)
    try:
        created = _send("POST", f"{base}/api/use-cases/{slug}/solve/jobs", key, body)
    except ApiError as error:
        if error.status == 404 and error.code in (None, "NOT_FOUND"):
            # The API answers a use case it does not serve with the framework's 404, as it would a wrong URL.
            raise ApiError(
                f"the optimaru API at {base} does not serve {slug}, or the API URL is wrong ({error})",
                status=404,
                code=error.code,
                detail=error.detail,
            ) from error
        if error.status is not None:
            raise
        # No answer: the API may have taken the job before the connection failed, and it runs to its
        # time limit. Creating it again could solve it twice, so it is not tried again.
        raise ApiError(
            f"{error}; if the request reached the API, a solve job was created and runs to its time limit",
            status=None,
            code=error.code,
        ) from error
    job = _job_of(created, base)
    status_url = base + job["status_url"]
    limit = _wait_limit(body)
    # A long solve rides out a longer outage: a tenth of its wait, and never less than the minute.
    patience = max(_POLL_PATIENCE, 0.1 * limit)
    started = time.monotonic()
    try:
        wait = _POLL_FIRST
        while job["status"] not in _FINAL_STATUSES:
            if time.monotonic() - started > limit:
                raise ApiError(
                    f"the solve job did not finish within {limit:g} s on the optimaru API "
                    f"(still {job['status']!r}); a cancel was sent. The wait follows the time limit "
                    "of the call: give the call, or the request, a longer time limit to wait longer",
                    code="JOB_TIMEOUT",
                )
            time.sleep(wait)
            wait = min(wait * 1.5, _POLL_LONGEST)
            # Within the job's own wait: a failing look does not carry the solve past it.
            left = max(limit - (time.monotonic() - started), _POLL_FIRST)
            job = _job_of(_patiently("GET", status_url, key, min(patience, left)), base)
        if job["status"] == "succeeded":
            return _patiently("GET", base + job["result_url"], key, patience)
    except BaseException:
        # Interrupted (Ctrl-C), given up or past its time: do not leave the job running on the account.
        # Cancelling a job that has ended changes nothing.
        try:
            _send("POST", f"{status_url}/cancel", key, timeout=_CANCEL_TIMEOUT)
        except Exception:
            pass
        raise
    if job["status"] == "cancelled":
        raise ApiError("the solve job was cancelled on the optimaru API", code="JOB_CANCELLED")
    raise ApiError(job.get("error") or "the solve job failed on the optimaru API", code="JOB_FAILED")


def _read(model: Any, result: Any, slug: str, base: Any = None) -> Any:
    """The API's result as `model`, the class the local call returns. `base` is the use case's own model class."""
    from pydantic import ValidationError

    try:
        return model.model_validate(result)
    except ValidationError as error:
        if base is not None and model is not base:
            raise ApiError(
                f"the {slug} result of the optimaru API is not a {model.__name__}: the API returns a "
                f"{base.__name__}, and a field {model.__name__} adds to it is not in the result"
            ) from error
        raise ApiError(
            f"the optimaru API returned a {slug} result that optimaru {_version()} cannot read; "
            "the API may run another version of the engine"
        ) from error


# ---------------------------------------------------------------------------
# What a use case decorates its entry points with
# ---------------------------------------------------------------------------


def _check_settings(package: str, keywords: dict[str, Any]) -> None:
    """Refuse here what the use case's ``SolverConfig`` refuses, before a job exists.

    The API's request model takes any integer as a seed, and the engine then fails the
    job without naming it; here the setter names it at once, as a local solve would.
    """
    import sys

    from ._solve import solver_config

    config_type = sys.modules[package].SolverConfig
    config = keywords.get("config")
    if callable(getattr(config, "to_core", None)):
        config = config.to_core()
    if config is not None and not isinstance(config, config_type):
        raise TypeError(f"config must be a {package}.SolverConfig, not a {type(config).__name__}")
    solver_config(
        config_type,
        config,
        time_limit_seconds=keywords.get("time_limit_seconds"),
        random_seed=keywords.get("random_seed"),
        parallel_solver_count=keywords.get("parallel_solver_count"),
    )


def _bound(function: Any, signature: Any, args: tuple, keywords: dict) -> tuple[Any, dict[str, Any]]:
    """The first argument and the keywords of a call, defaults filled in. A wrong call raises as the function would."""
    try:
        call = signature.bind(*args, **keywords)
    except TypeError:
        # The same call fails in Python's own binding, before the body runs, and its message names
        # the function and the argument.
        function(*args, **keywords)
        raise
    call.apply_defaults()
    first, *rest = call.arguments.items()
    return first[1], dict(rest)


def model_solve(slug: str, model: type, *, dicts: bool = False) -> Callable[[Callable[_P, _R]], Callable[_P, _R]]:
    """The ``solve()`` of a use case the API solves with ``solve``: its problem is `model` or the compiled twin.

    With the API on, a `model` goes to ``POST /api/use-cases/<slug>/solve/jobs`` as
    ``{"problem": <the model>, <keyword>: ...}`` and the result comes back as a new
    `model`, which is what ``solve(model)`` returns here too. A compiled problem is
    solved in place, which a result that comes back as JSON cannot do: it is refused,
    naming the way out. `dicts` is for a ``solve()`` that also takes a dict of the
    model's shape and returns the solved model for it.
    """

    def decorate(solve: Callable[_P, _R]) -> Callable[_P, _R]:
        import inspect

        signature = inspect.signature(solve)

        @functools.wraps(solve)
        def solve_here_or_on_the_api(*args: _P.args, **keywords: _P.kwargs) -> _R:
            settings = _settings()
            if settings is None:
                return solve(*args, **keywords)
            problem, rest = _bound(solve, signature, args, keywords)
            if dicts and isinstance(problem, dict):
                problem = model.model_validate(problem)
            if not isinstance(problem, model):
                raise TypeError(
                    f"with the optimaru API on, {solve.__module__}.solve() takes a {model.__name__}, not a "
                    f"{type(problem).__name__}. A compiled problem is solved in place, which the API cannot do: "
                    f"pass its model ({model.__name__}.from_core(problem)) and use the model that comes back, "
                    "or call optimaru.use_local() to solve on this machine"
                )
            _check_settings(solve.__module__, rest)
            body = {"problem": _model_json(problem), **_solve_body(rest)}
            # The caller's own class, so that a subclass of the model comes back as one, as it does here.
            return _read(type(problem), _solve_job(settings, slug, body), slug, model)

        return solve_here_or_on_the_api

    return decorate


def request_solve(
    slug: str, result: str, *, prepare: Callable[[Any, Any], Any] | None = None
) -> Callable[[Callable[_P, _R]], Callable[_P, _R]]:
    """The ``solve_request()`` of a use case the API solves with ``solve_request``.

    With the API on, the request goes to ``POST /api/use-cases/<slug>/solve/jobs`` as
    ``{"request": <the request>, "constraints": [...]}`` and the result comes back as the
    model named `result` in the function's module, its response model.

    `prepare` is for a use case whose selection travels inside the request (employee
    scheduling): a function ``(request, constraints) -> request`` that returns the request
    to send, with a ``constraints=`` selection (or None) put where the API reads it.
    """

    def decorate(solve_request: Callable[_P, _R]) -> Callable[_P, _R]:
        import inspect
        import sys

        signature = inspect.signature(solve_request)
        # Every solve_request is defined in optimaru.<use case>.models; its package holds the SolverConfig.
        package, _, module = solve_request.__module__.rpartition(".")
        if module != "models" or not package.startswith("optimaru."):
            raise TypeError(
                f"request_solve decorates a solve_request of optimaru.<use case>.models, not of {solve_request.__module__}"
            )

        @functools.wraps(solve_request)
        def solve_here_or_on_the_api(*args: _P.args, **keywords: _P.kwargs) -> _R:
            settings = _settings()
            if settings is None:
                return solve_request(*args, **keywords)
            request, rest = _bound(solve_request, signature, args, keywords)
            if not callable(getattr(request, "model_dump", None)):
                raise TypeError(
                    f"{solve_request.__module__}.solve_request() takes a request model, not a {type(request).__name__}"
                )
            picked = rest.pop("constraints", None)
            for name, value in rest.items():
                if value is not None:
                    raise ValueError(f"{name} cannot be sent to the optimaru API")
            body: dict[str, Any] = {}
            if prepare is not None:
                request = prepare(request, picked)
            elif picked is not None:
                body["constraints"] = _constraints_json(picked)
            body = {"request": _model_json(request), **body}
            # The seed as sent, which is the one it solves with (employee scheduling's request writes
            # the seed of its settings there while its own is not given).
            _check_settings(package, {"random_seed": body["request"].get("random_seed")})
            answer = _solve_job(settings, slug, body)
            return _read(getattr(sys.modules[solve_request.__module__], result), answer, slug)

        return solve_here_or_on_the_api

    return decorate


def compiled_solve(solve: Callable[_P, _R]) -> Callable[_P, _R]:
    """The ``solve()`` of a use case whose ``solve()`` takes compiled objects only.

    Such a problem is solved in place and has no JSON to send, so with the API on the
    call is refused, naming ``solve_request()``, which the API does solve.
    """

    @functools.wraps(solve)
    def solve_here_only(*args: _P.args, **keywords: _P.kwargs) -> _R:
        if _api_on():
            raise TypeError(
                f"with the optimaru API on, {solve.__module__}.solve() cannot run: it solves a compiled "
                "problem in place, which the API cannot do. Call solve_request() with a request model, or "
                "call optimaru.use_local() to solve on this machine"
            )
        return solve(*args, **keywords)

    return solve_here_only


def local_solve(use_case: str) -> Callable[[Callable[_P, _R]], Callable[_P, _R]]:
    """The ``solve()`` of a use case the API does not serve: it solves on this machine either way.

    With the API on it says so once per process, as a warning, so that nobody takes a solve
    that ran here for one that ran there.
    """

    def decorate(solve: Callable[_P, _R]) -> Callable[_P, _R]:
        @functools.wraps(solve)
        def solve_here(*args: _P.args, **keywords: _P.kwargs) -> _R:
            said = f"local:{use_case}"
            if said not in _said and _api_on():
                import warnings

                _said.add(said)
                warnings.warn(
                    f"the optimaru API does not take the Python call of {use_case}: it is solved on this machine",
                    RuntimeWarning,
                    stacklevel=2,
                )
            return solve(*args, **keywords)

        return solve_here

    return decorate

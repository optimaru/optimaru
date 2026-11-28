# SPDX-License-Identifier: Apache-2.0
"""How a `SolverConfig` is laid out: its settings in groups, one per stage of a solve.

A solve runs in stages, and every setting belongs to one of them::

    construction   the first solution s0
    stop           when the search stops
    cost           what a solution is scored on (beyond the `constraints=` of solve)
    alns_trigger   when the search counts as stalled and a ruin & recreate kick runs
    strategies     a step's move, first: a built-in strategy drawn by its weight
    custom_moves   a step's move, next: one of the use case's own moves
    moves          a step's move, otherwise: the move mix
    selection      what the move mix moves: nearby candidates and the use case's hints
    acceptance     whether the candidate s' becomes the current solution
    alns           the kick itself: which ruins, how much, how it is rebuilt
    run            seeds, parallel starts and debugging aids

so a setting is read and written as ``config.<group>.<name>``::

    config = vehicle_routing.SolverConfig()
    config.stop.time_limit_seconds = 10
    config.moves.two_opt_weight = 0.2
    config.alns.min_ruin_fraction = 0.05

or given per group when the config is made::

    vehicle_routing.SolverConfig(stop={"time_limit_seconds": 10}, alns={"min_ruin_fraction": 0.05})

`FIELDS` below is the one table of where each setting lives. The compiled
``SolverConfig`` of each package keeps its own (flat) field names; this module
puts the groups in front of it, and refuses the flat names with a message naming
the new place. Nothing here runs during a solve.
"""

from __future__ import annotations

from typing import Any

__all__ = ["FIELDS", "GROUPS", "config_fields", "group_of", "grouped", "set_nested"]

#: The groups, in the order a solve meets them (also the order of `repr` and `to_dict`).
GROUPS: tuple[str, ...] = (
    "construction",
    "stop",
    "cost",
    "alns_trigger",
    "strategies",
    "custom_moves",
    "moves",
    "selection",
    "acceptance",
    "alns",
    "run",
)

#: Every compiled field: its group and its name there. A compiled field missing here
#: fails `grouped` (and the layout test), so a new setting has to be placed.
FIELDS: dict[str, tuple[str, str]] = {
    # construction: the first solution
    "first_fit_decreasing": ("construction", "first_fit_decreasing"),
    "pair_construction_heuristic": ("construction", "pair_heuristic"),
    "employee_greedy_construction": ("construction", "employee_greedy"),
    "greedy_repair_budget_fraction": ("construction", "greedy_repair_budget_fraction"),
    # stop: the first limit reached ends the search
    "spent_limit_seconds": ("stop", "time_limit_seconds"),
    "unimproved_spent_limit_seconds": ("stop", "unimproved_time_limit_seconds"),
    "step_count_limit": ("stop", "step_limit"),
    "unimproved_step_count_limit": ("stop", "unimproved_step_limit"),
    "best_score_limit": ("stop", "target_score"),
    # cost: the objective's own settings (the constraints themselves go to solve(constraints=...))
    "weights": ("cost", "weights"),
    # alns_trigger: when a kick runs
    "ruin_recreate": ("alns_trigger", "enabled"),
    "ruin_recreate_unimproved_step_trigger": ("alns_trigger", "unimproved_steps"),
    # strategies: drawn first, each by its own weight, in the engine's order
    "route_elimination_weight": ("strategies", "route_elimination_weight"),
    "route_elimination_max_size": ("strategies", "route_elimination_max_size"),
    "ejection_chain_weight": ("strategies", "ejection_chain_weight"),
    "ejection_chain_max_steps": ("strategies", "ejection_chain_max_steps"),
    "ejection_chain_candidate_lists": ("strategies", "ejection_chain_candidate_lists"),
    "ejection_chain_max_evaluations": ("strategies", "ejection_chain_max_evaluations"),
    "iterated_local_search_weight": ("strategies", "iterated_local_search_weight"),
    "iterated_local_search_min_ruin": ("strategies", "iterated_local_search_min_ruin"),
    "iterated_local_search_max_ruin": ("strategies", "iterated_local_search_max_ruin"),
    "iterated_local_search_max_segment": ("strategies", "iterated_local_search_max_segment"),
    "iterated_local_search_max_evaluations": ("strategies", "iterated_local_search_max_evaluations"),
    "descent_weight": ("strategies", "descent_weight"),
    "descent_max_sweeps": ("strategies", "descent_max_sweeps"),
    "granular_descent": ("strategies", "granular_descent"),
    "granular_descent_max_segment": ("strategies", "granular_descent_max_segment"),
    "granular_descent_candidate_routes": ("strategies", "granular_descent_candidate_routes"),
    "ruin_recreate_as_move_weight": ("strategies", "ruin_recreate_weight"),
    "ruin_recreate_as_move_max_ruin": ("strategies", "ruin_recreate_max_ruin"),
    # custom_moves: the use case's own moves, their switches and settings
    "custom_move_probability": ("custom_moves", "share"),
    "bundle_moves": ("custom_moves", "bundle"),
    "bundle_max_requests": ("custom_moves", "bundle_max_requests"),
    "close_box_moves": ("custom_moves", "close_box"),
    "close_facility_moves": ("custom_moves", "close_facility"),
    "close_facility_keep": ("custom_moves", "close_facility_keep"),
    "close_facility_migrate_probability": ("custom_moves", "close_facility_migrate_probability"),
    "close_station_moves": ("custom_moves", "close_station"),
    "core_repack_moves": ("custom_moves", "core_repack"),
    "employee_day_repair_moves": ("custom_moves", "employee_day_repair"),
    "employee_run_repair_moves": ("custom_moves", "employee_run_repair"),
    "employee_run_repair_max_days": ("custom_moves", "employee_run_repair_max_days"),
    "employee_unassign_moves": ("custom_moves", "employee_unassign"),
    "employee_window_moves": ("custom_moves", "employee_window"),
    "employee_window_max_days": ("custom_moves", "employee_window_max_days"),
    "empty_bin_moves": ("custom_moves", "empty_bin"),
    "empty_bin_candidates": ("custom_moves", "empty_bin_candidates"),
    "empty_container_moves": ("custom_moves", "empty_container"),
    "empty_container_candidates": ("custom_moves", "empty_container_candidates"),
    "guided_moves": ("custom_moves", "guided"),
    "open_box_moves": ("custom_moves", "open_box"),
    "order_consolidate_moves": ("custom_moves", "order_consolidate"),
    "pair_moves": ("custom_moves", "pair"),
    "pair_assignment_probability": ("custom_moves", "pair_assignment_probability"),
    "pair_repair_probability": ("custom_moves", "pair_repair_probability"),
    "pair_exchange_moves": ("custom_moves", "pair_exchange"),
    "pair_exchange_min_nodes": ("custom_moves", "pair_exchange_min_nodes"),
    "priority_swap_moves": ("custom_moves", "priority_swap"),
    "reassign_candidates": ("custom_moves", "reassign_candidates"),
    "repack_moves": ("custom_moves", "repack"),
    "repack_partner_bins": ("custom_moves", "repack_partner_bins"),
    "serial_schedule_moves": ("custom_moves", "serial_schedule"),
    "smart_change_moves": ("custom_moves", "smart_change"),
    "smart_change_candidate_count": ("custom_moves", "smart_change_candidate_count"),
    "smart_class_swap_moves": ("custom_moves", "smart_class_swap"),
    "smart_class_swap_candidates": ("custom_moves", "smart_class_swap_candidates"),
    "stop_order_moves": ("custom_moves", "stop_order"),
    "swap_box_moves": ("custom_moves", "swap_box"),
    "swap_facility_moves": ("custom_moves", "swap_facility"),
    "switch_container_moves": ("custom_moves", "switch_container"),
    "team_count_moves": ("custom_moves", "team_count"),
    "trade_moves": ("custom_moves", "trade"),
    # moves: the move mix, by relative weight (0 turns a move off)
    "change_move_weight": ("moves", "change_weight"),
    "swap_move_weight": ("moves", "swap_weight"),
    "relocate_move_weight": ("moves", "relocate_weight"),
    "list_swap_move_weight": ("moves", "list_swap_weight"),
    "two_opt_move_weight": ("moves", "two_opt_weight"),
    "or_opt_move_weight": ("moves", "or_opt_weight"),
    "tail_swap_move_weight": ("moves", "tail_swap_weight"),
    "inter_exchange_move_weight": ("moves", "inter_exchange_weight"),
    "inter_exchange_max_segment": ("moves", "inter_exchange_max_segment"),
    # selection: what the move mix moves
    "nearby_size": ("selection", "nearby_size"),
    "nearby_probability": ("selection", "nearby_probability"),
    "nearby_shifts": ("selection", "nearby_shifts"),
    "nearby_shifts_probability": ("selection", "nearby_shifts_probability"),
    "distance_nearby": ("selection", "distance_nearby"),
    "distance_nearby_size": ("selection", "distance_nearby_size"),
    "distance_nearby_probability": ("selection", "distance_nearby_probability"),
    "driving_time_nearby": ("selection", "driving_time_nearby"),
    "driving_time_nearby_size": ("selection", "driving_time_nearby_size"),
    "driving_time_nearby_probability": ("selection", "driving_time_nearby_probability"),
    "same_class_nearby": ("selection", "same_class_nearby"),
    "same_class_nearby_probability": ("selection", "same_class_nearby_probability"),
    "same_group_nearby": ("selection", "same_group_nearby"),
    "same_group_nearby_probability": ("selection", "same_group_nearby_probability"),
    "value_recommender": ("selection", "value_recommender"),
    "value_recommender_keep": ("selection", "value_recommender_keep"),
    "value_recommender_probability": ("selection", "value_recommender_probability"),
    "compatible_value_probability": ("selection", "compatible_value_probability"),
    "day_fit_value_probability": ("selection", "day_fit_value_probability"),
    "window_value_probability": ("selection", "window_value_probability"),
    "hot_job_probability": ("selection", "hot_job_probability"),
    "hot_lesson_probability": ("selection", "hot_lesson_probability"),
    "hot_talk_probability": ("selection", "hot_talk_probability"),
    "costliest_match_sample": ("selection", "costliest_match_sample"),
    "machine_move_probability": ("selection", "machine_move_probability"),
    "free_crew_candidates": ("selection", "free_crew_candidates"),
    # acceptance: the rule and its parameters
    "acceptor_type": ("acceptance", "rule"),
    "late_acceptance_size": ("acceptance", "late_acceptance_size"),
    "entity_tabu_size": ("acceptance", "entity_tabu_size"),
    "simulated_annealing_starting_temperature": ("acceptance", "simulated_annealing_starting_temperature"),
    "simulated_annealing_temperature_fraction": ("acceptance", "simulated_annealing_temperature_fraction"),
    "great_deluge_water_level_step": ("acceptance", "great_deluge_water_level_step"),
    "step_counting_hill_climbing_size": ("acceptance", "step_counting_hill_climbing_size"),
    "record_to_record_start_gap": ("acceptance", "record_to_record_start_gap"),
    "record_to_record_end_gap": ("acceptance", "record_to_record_end_gap"),
    "restart_segments": ("acceptance", "restart_segments"),
    "fill_tie_break": ("acceptance", "fill_tie_break"),
    "reheat": ("acceptance", "reheat"),
    # alns: the kick
    "ruin_recreate_min_ruin_fraction": ("alns", "min_ruin_fraction"),
    "ruin_recreate_max_ruin_fraction": ("alns", "max_ruin_fraction"),
    "ruin_recreate_max_ruin_count": ("alns", "max_ruin_count"),
    "ruin_recreate_polish_steps": ("alns", "polish_steps"),
    "ruin_recreate_operator_decay": ("alns", "operator_decay"),
    "ruin_recreate_rewards": ("alns", "rewards"),
    "ruin_recreate_selection_type": ("alns", "selection_type"),
    "ruin_recreate_selection_segment_length": ("alns", "selection_segment_length"),
    "ruin_recreate_selection_alpha": ("alns", "selection_alpha"),
    "sisr_ruin": ("alns", "sisr_ruin"),
    "sisr_max_string_removals": ("alns", "sisr_max_string_removals"),
    "sisr_max_string_size": ("alns", "sisr_max_string_size"),
    "route_ruin": ("alns", "route_ruin"),
    "group_ruin_recreate": ("alns", "group_ruin"),
    "group_ruin_max_visits": ("alns", "group_ruin_max_visits"),
    "request_ruin_recreate": ("alns", "request_ruin"),
    "request_ruin_max_requests": ("alns", "request_ruin_max_requests"),
    "pair_ruin": ("alns", "pair_ruin"),
    "employee_ruin_moves": ("alns", "employee_ruin"),
    "regret_k": ("alns", "regret_k"),
    "blink_rate": ("alns", "blink_rate"),
    # run
    "random_seed": ("run", "random_seed"),
    "parallel_solver_count": ("run", "parallel_solver_count"),
    "operator_rounds": ("run", "operator_rounds"),
    "full_assert": ("run", "full_assert"),
    "on_new_best": ("run", "on_new_best"),
}


def group_of(flat: str) -> tuple[str, str]:
    """The group and the name there of a compiled field."""
    return FIELDS[flat]


def config_fields(compiled: type) -> list[str]:
    """The settings of a compiled ``SolverConfig`` class, in its own order."""
    return [name for name, value in vars(compiled).items() if isinstance(value, property) and not name.startswith("_")]


class _Group:
    """One group of a config: ``config.alns``. Reads and writes go to the config itself."""

    __slots__ = ("_config", "_group", "_names")

    def __init__(self, config: Any, group: str, names: dict[str, str]) -> None:
        object.__setattr__(self, "_config", config)
        object.__setattr__(self, "_group", group)
        object.__setattr__(self, "_names", names)  # name in the group -> compiled field

    def _flat(self, name: str) -> str:
        try:
            return self._names[name]
        except KeyError:
            options = ", ".join(self._names) or "none"
            raise AttributeError(f"SolverConfig.{self._group} has no setting {name!r} (it has: {options})") from None

    def __getattr__(self, name: str) -> Any:
        if name.startswith("__"):
            raise AttributeError(name)
        return _raw_get(self._config, self._flat(name))

    def __setattr__(self, name: str, value: Any) -> None:
        flat = self._flat(name)
        try:
            _raw_set(self._config, flat, value)
        except (TypeError, ValueError) as error:
            # The compiled setters name the compiled field; the caller wrote the grouped one.
            message = str(error).replace(flat, f"{self._group}.{name}")
            if message == str(error):
                raise
            raise type(error)(message) from None

    def __dir__(self) -> list[str]:
        return list(self._names)

    def to_dict(self) -> dict[str, Any]:
        """This group's settings by name (``on_new_best`` left out)."""
        return {name: _raw_get(self._config, flat) for name, flat in self._names.items() if name != "on_new_best"}

    def __repr__(self) -> str:
        body = ", ".join(f"{k}={v!r}" for k, v in self.to_dict().items())
        return f"{self._group}({body})"

    def __eq__(self, other: object) -> bool:
        return isinstance(other, _Group) and self._group == other._group and self.to_dict() == other.to_dict()

    __hash__ = None  # type: ignore[assignment]


def _raw_get(config: Any, flat: str) -> Any:
    return type(config)._optimaru_raw[flat].__get__(config)


def _raw_set(config: Any, flat: str, value: Any) -> None:
    type(config)._optimaru_raw[flat].__set__(config, value)


class _GroupAccess:
    """The class attribute behind ``config.<group>``."""

    def __init__(self, group: str, names: dict[str, str], doc: str) -> None:
        self.group = group
        self.names = names
        self.__doc__ = doc

    def __get__(self, config: Any, owner: type | None = None) -> Any:
        if config is None:
            return self
        return _Group(config, self.group, self.names)

    def __set__(self, config: Any, values: Any) -> None:
        if isinstance(values, _Group):
            values = values.to_dict()
        if not isinstance(values, dict):
            raise TypeError(f"SolverConfig.{self.group} takes a dict of its settings, not {type(values).__name__}")
        view = _Group(config, self.group, self.names)
        for name, value in values.items():
            setattr(view, name, value)


def grouped(compiled: type, *, module: str | None = None) -> type:
    """Lay out a package's compiled ``SolverConfig`` by group, in place, and return it.

    The class itself changes, so every config is grouped, wherever it is made (by the
    caller, by the package, or returned from compiled code), and a `Solver` takes it as
    before. Each setting is reached through its group (`GROUPS`); a compiled (flat)
    name is removed from the class and raises AttributeError naming its new place.
    """
    if "_optimaru_raw" in compiled.__dict__:
        return compiled
    fields = config_fields(compiled)
    missing = [f for f in fields if f not in FIELDS]
    if missing:
        raise RuntimeError(f"{compiled.__module__}.{compiled.__name__}: no group for {', '.join(missing)}")
    layout: dict[str, dict[str, str]] = {}
    present = set(fields)
    for flat, (group, short) in FIELDS.items():  # the table's order, not the compiled one
        if flat in present:
            layout.setdefault(group, {})[short] = flat
    groups = tuple(g for g in GROUPS if g in layout)
    raw = {flat: compiled.__dict__[flat] for flat in fields}
    for flat in fields:
        delattr(compiled, flat)
    compiled_init = compiled.__init__

    def _unknown(attr: str) -> str:
        if attr in FIELDS:
            group, short = FIELDS[attr]
            return f"SolverConfig.{attr} is now SolverConfig.{group}.{short}"
        return f"SolverConfig has no group {attr!r} (it has: {', '.join(groups)})"

    def __init__(self, **values: Any) -> None:
        compiled_init(self)
        for group, settings in values.items():
            if group not in layout:
                raise TypeError(_unknown(group))
            setattr(self, group, settings)

    def __getattr__(self, attr: str) -> Any:  # only reached for a name the class does not have
        if attr.startswith("__"):
            raise AttributeError(attr)
        raise AttributeError(_unknown(attr))

    def __setattr__(self, attr: str, value: Any) -> None:
        if attr not in layout:
            raise AttributeError(_unknown(attr))
        object.__setattr__(self, attr, value)  # through _GroupAccess.__set__

    def to_dict(self) -> dict[str, dict[str, Any]]:
        """Every setting, by group and name: the shape of a request's ``solver_parameters``."""
        return {g: getattr(self, g).to_dict() for g in groups}

    def __repr__(self) -> str:
        return "SolverConfig(" + ", ".join(repr(getattr(self, g)) for g in groups) + ")"

    def __eq__(self, other: object) -> bool:
        return type(other) is type(self) and self.to_dict() == other.to_dict()

    members: dict[str, Any] = {
        "_optimaru_raw": raw,
        "GROUPS": groups,
        "__init__": __init__,
        "__getattr__": __getattr__,
        "__setattr__": __setattr__,
        "__repr__": __repr__,
        "__eq__": __eq__,
        "__hash__": None,
        "to_dict": to_dict,
    }
    for group in groups:
        names = layout[group]
        members[group] = _GroupAccess(group, names, f"{group}: " + ", ".join(names))
    for key, value in members.items():
        setattr(compiled, key, value)
    if module:
        compiled.__module__ = module
    compiled.__doc__ = (
        (compiled.__doc__ or "")
        + "\n\nSettings by stage of the solve: "
        + ", ".join(f"``{g}``" for g in groups)
        + ". See `optimaru._config`."
    )
    return compiled


def set_nested(config: Any, parameters: dict[str, Any], *, refused: frozenset[str] = frozenset()) -> None:
    """Apply ``{"group": {"name": value}}`` to a config (a request's ``solver_parameters``).

    A group or a name that does not exist raises ValueError naming it (a flat name of an
    earlier version names its new place); ``refused`` holds the ``group.name`` settings the
    caller controls elsewhere. ``on_new_best`` is a callback and never a parameter. ``None``
    leaves a setting as it is, a list becomes the ``stop.target_score`` tuple, and the name
    of an enum member becomes the member.
    """
    groups = getattr(type(config), "GROUPS", ())
    for group, settings in parameters.items():
        if group not in groups:
            if group in FIELDS:
                g, n = FIELDS[group]
                raise ValueError(f"solver parameter {group!r} is now {g}.{n}: send {{{g!r}: {{{n!r}: ...}}}}")
            raise ValueError(f"unknown solver parameter group: {group} (the groups are: {', '.join(groups)})")
        if not isinstance(settings, dict):
            raise ValueError(f"solver parameter group {group!r} takes an object of settings")
        view = getattr(config, group)
        for name, value in settings.items():
            if f"{group}.{name}" in refused:
                raise ValueError(f"solver parameter {group}.{name} is controlled by the solve request")
            if name == "on_new_best" or name not in dir(view):
                raise ValueError(f"unknown solver parameter: {group}.{name}")
            if value is None:
                continue
            current = getattr(view, name)
            if name == "target_score" and isinstance(value, list):
                value = tuple(value)
            elif isinstance(value, str) and hasattr(type(current), value):
                value = getattr(type(current), value)
            try:
                setattr(view, name, value)
            except (TypeError, ValueError) as exc:
                raise ValueError(f"invalid solver parameter {group}.{name}: {exc}") from exc

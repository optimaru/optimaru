"""Seeded random semiconductor-scheduling instances as plain JSON dicts (test helper).

Every instance passes ``SemiconductorScheduleModel`` validation and exercises the quirks the
port must keep: re-entrant routes, repeated machine options (the scorer reads the last one),
batch tools with a minimum size, duplicate setup / transport entries (the last one wins),
machine and resource downtime, duplicated required resources, pins, and assignments that
break every rule (ineligible machines, wrong durations, overlaps, missing operations,
``batch_id`` values that are None, empty or shared).
"""

from __future__ import annotations

import random

FAMILIES = ("A", "B", "C")
GROUPS = ("G1", "G2", "G3")


def _windows(rng: random.Random, count: int, horizon: int = 40) -> list[dict]:
    windows = []
    for _ in range(count):
        start = rng.randint(0, horizon)
        windows.append({"start": start, "end": start + rng.randint(1, 6)})
    return windows


def instance(seed: int) -> dict:
    """One random problem with a random ``assignments`` list (not a decoded schedule)."""
    rng = random.Random(seed)
    machines = []
    for index in range(rng.randint(2, 5)):
        capacity = rng.choice((1, 1, 2, 3))
        machines.append(
            {
                "id": f"M{index}",
                "group": rng.choice(GROUPS),
                "batch_capacity": capacity,
                "minimum_batch_size": rng.randint(1, capacity),
                "unavailable": _windows(rng, rng.choice((0, 0, 1, 2))),
            }
        )
    resources = [
        {"id": f"R{index}", "capacity": rng.randint(1, 2), "unavailable": _windows(rng, rng.choice((0, 1, 2)))}
        for index in range(rng.choice((0, 1, 2)))
    ]
    batch_duration: dict[tuple[str, str], int] = {}
    lots = []
    for lot_index in range(rng.randint(1, 5)):
        operations = []
        for op_index in range(rng.randint(1, 4)):
            family = rng.choice(FAMILIES)
            options = []
            for _ in range(rng.choice((1, 1, 2, 3))):
                machine = rng.choice(machines)
                duration = rng.randint(1, 6)
                if machine["batch_capacity"] > 1:
                    duration = batch_duration.setdefault((machine["id"], family), duration)
                options.append(
                    {"machine_id": machine["id"], "duration": duration, "preference_cost": rng.choice((0, 0, 1, 3))}
                )
            required = [rng.choice(resources)["id"] for _ in range(rng.choice((0, 0, 1, 2)))] if resources else []
            operations.append(
                {
                    "id": f"O{op_index}",
                    "family": family,
                    "eligible_machines": options,
                    "required_resources": required,
                    "min_queue_time": rng.choice((0, 0, 1, 3)),
                    "max_queue_time": rng.choice((None, None, 0, 2, 5)),
                }
            )
        lots.append(
            {
                "id": f"L{rng.randint(0, 99):02d}-{lot_index}",
                "product": "P",
                "release_time": rng.randint(0, 5),
                "due_time": rng.randint(5, 40),
                "priority": rng.randint(1, 3),
                "max_cycle_time": rng.choice((None, 5, 15, 30)),
                "operations": operations,
            }
        )
    setup_times = [
        {
            "machine_group": rng.choice(GROUPS),
            "from_family": rng.choice(FAMILIES),
            "to_family": rng.choice(FAMILIES),
            "duration": rng.randint(0, 4),
        }
        for _ in range(rng.randint(0, 6))
    ]
    transport_times = [
        {"from_group": rng.choice(GROUPS), "to_group": rng.choice(GROUPS), "duration": rng.randint(0, 4)}
        for _ in range(rng.randint(0, 5))
    ]
    keys = [(lot, op) for lot in lots for op in lot["operations"]]
    pinned = []
    for lot, op in rng.sample(keys, min(len(keys), rng.choice((0, 0, 1, 2)))):
        option = {item["machine_id"]: item for item in op["eligible_machines"]}[
            rng.choice(op["eligible_machines"])["machine_id"]
        ]
        start = rng.randint(0, 20)
        pinned.append(
            {
                "lot_id": lot["id"],
                "operation_id": op["id"],
                "machine_id": option["machine_id"],
                "start": start,
                "end": start + option["duration"],
            }
        )
    assignments = []
    for lot, op in keys:
        if rng.random() < 0.15:
            continue
        if rng.random() < 0.8:
            option = rng.choice(op["eligible_machines"])
            machine_id, duration = option["machine_id"], option["duration"]
        else:
            machine_id, duration = rng.choice(machines)["id"], rng.randint(1, 6)
        start = rng.randint(0, 30)
        assignments.append(
            {
                "lot_id": lot["id"],
                "operation_id": op["id"],
                "machine_id": machine_id,
                "start": start,
                "end": start + duration,
                "batch_id": rng.choice((None, None, "", "RUN1", "RUN2", f"B{rng.randint(1, 3):03d}")),
            }
        )
    rng.shuffle(assignments)
    return {
        "machines": machines,
        "auxiliary_resources": resources,
        "setup_times": setup_times,
        "transport_times": transport_times,
        "lots": lots,
        "pinned_assignments": pinned,
        "assignments": assignments,
    }


def search_state(seed: int, row_count: int, option_counts: list[int]) -> tuple[list[int], list[int]]:
    """A random (choices, ranks) state: choices may exceed the option count (used modulo), ranks may tie."""
    rng = random.Random(f"state:{seed}")
    choices = [rng.randint(0, 3 * max(1, count)) for count in option_counts]
    if rng.random() < 0.5:
        ranks = list(range(row_count))
        rng.shuffle(ranks)
    else:
        ranks = [rng.randint(0, max(0, row_count - 1)) for _ in range(row_count)]
    return choices, ranks

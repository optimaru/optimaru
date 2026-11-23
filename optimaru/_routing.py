"""Depots and travel profiles, shared by vehicle routing and pickup-delivery.

A routing problem names its places by id: each depot (`DepotModel`) and each place a
vehicle stops at. A vehicle starts at one depot and, unless its route is open, ends at
one (the same or another). How far and how long a drive takes comes from the vehicle's
travel profile (`TravelProfile`): pairwise driving seconds and driving meters keyed by
those ids, ``{from_id: {to_id: value}}``. A pair the dicts leave out costs the profile's
``missing_value``; a place to itself costs 0 unless given. A problem without profiles,
or a profile without one of the two dicts, drives straight lines between the
coordinates (haversine meters, at 50 km/h for the seconds).

The use cases turn the dicts into one square matrix per profile before the solve
(`profile_matrices`): the engine reads matrices, the JSON stays keyed by id.
"""

from collections.abc import Iterable, Sequence

from pydantic import Field, model_validator

from ._models import StrictModel

__all__ = [
    "DEFAULT_PROFILE",
    "MISSING_TRAVEL_VALUE",
    "DepotModel",
    "TravelProfile",
    "check_depots_and_profiles",
    "profile_matrices",
]

# What a pair the dicts leave out costs, in seconds or meters: about 116 days or
# 10,000 km, so a route avoids such a leg wherever it can, and a leg it cannot avoid
# misses any time window of the day.
MISSING_TRAVEL_VALUE = 10_000_000

# The profile a vehicle drives on when it names none.
DEFAULT_PROFILE = "default"


class DepotModel(StrictModel):
    """A place where routes start or end."""

    id: str
    location: tuple[float, float]  # (latitude, longitude)


class TravelProfile(StrictModel):
    """Pairwise travel of one kind of vehicle, keyed by place id."""

    durations: dict[str, dict[str, int]] | None = Field(
        default=None,
        description=(
            "Driving seconds, `{from_id: {to_id: seconds}}` over depot and stop ids. "
            "None drives straight lines at 50 km/h."
        ),
    )
    distances: dict[str, dict[str, int]] | None = Field(
        default=None,
        description=(
            "Driving meters, `{from_id: {to_id: meters}}` over depot and stop ids. "
            "None drives straight lines (haversine)."
        ),
    )
    missing_value: int = Field(
        default=MISSING_TRAVEL_VALUE,
        ge=0,
        description=(
            "What a pair the two dicts leave out costs, in seconds or meters. A place to itself costs 0 unless given."
        ),
    )

    @model_validator(mode="after")
    def _non_negative(self):
        for kind, pairs in (("durations", self.durations), ("distances", self.distances)):
            if pairs and any(row and min(row.values()) < 0 for row in pairs.values()):
                raise ValueError(f"{kind} values must be non-negative")
        return self


def check_depots_and_profiles(
    depots: Sequence[DepotModel],
    stop_ids: Iterable[str],
    vehicles: Sequence,
    profiles: dict[str, TravelProfile],
) -> None:
    """Refuse duplicate place ids, unknown depots or profiles, and pairs keyed by unknown ids.

    ``vehicles`` are the use case's vehicle models: each has ``id``, ``start_depot``,
    ``end_depot`` and ``profile``.
    """
    stop_ids = list(stop_ids)
    depot_ids = [depot.id for depot in depots]
    if len(depot_ids) != len(set(depot_ids)):
        raise ValueError("duplicate depot ids")
    if shared := sorted(set(depot_ids) & set(stop_ids)):
        raise ValueError(f"depots and stops share the ids {shared}: every place needs an id of its own")
    known_depots = set(depot_ids)
    for vehicle in vehicles:
        for field in ("start_depot", "end_depot"):
            depot = getattr(vehicle, field)
            if depot is not None and depot not in known_depots:
                raise ValueError(f"vehicle {vehicle.id}: unknown {field} {depot!r}")
        if profiles and vehicle.profile not in profiles:
            raise ValueError(
                f"vehicle {vehicle.id}: unknown profile {vehicle.profile!r} (profiles holds {sorted(profiles)})"
            )
        if not profiles and vehicle.profile != DEFAULT_PROFILE:
            raise ValueError(f"vehicle {vehicle.id}: profile {vehicle.profile!r} is given, but no profiles are")
    places = known_depots.union(stop_ids)
    for name, profile in profiles.items():
        for kind, pairs in (("durations", profile.durations), ("distances", profile.distances)):
            if not pairs:
                continue
            unknown = pairs.keys() - places
            for row in pairs.values():
                if not row.keys() <= places:
                    unknown |= row.keys() - places
            if unknown:
                raise ValueError(f"profile {name!r}: {kind} name unknown places {sorted(unknown)[:10]}")


def _matrix(pairs: dict[str, dict[str, int]], place_ids: Sequence[str], missing: int) -> list[list[int]]:
    index = {place: i for i, place in enumerate(place_ids)}
    rows = []
    for i, place in enumerate(place_ids):
        row = [missing] * len(place_ids)
        row[i] = 0
        for other, value in pairs.get(place, {}).items():
            row[index[other]] = value
        rows.append(row)
    return rows


def profile_matrices(
    profiles: dict[str, TravelProfile], place_ids: Sequence[str]
) -> tuple[dict[str, int], list[list[list[int]] | None], list[list[list[int]] | None]]:
    """The engine's view of the profiles: an index per profile name and per-profile matrices.

    The matrices are square over ``place_ids`` (rows are "from", columns "to"); None for a
    dict the profile does not give, which the engine fills from the coordinates. Without
    profiles there is one, `DEFAULT_PROFILE`, on straight lines.
    """
    if not profiles:
        return {DEFAULT_PROFILE: 0}, [None], [None]
    durations, distances = [], []
    for profile in profiles.values():
        durations.append(
            None if profile.durations is None else _matrix(profile.durations, place_ids, profile.missing_value)
        )
        distances.append(
            None if profile.distances is None else _matrix(profile.distances, place_ids, profile.missing_value)
        )
    return {name: i for i, name in enumerate(profiles)}, durations, distances

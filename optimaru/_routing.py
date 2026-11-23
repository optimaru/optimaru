"""Depots and travel profiles, shared by vehicle routing and pickup-delivery.

A routing problem names its places by id: each depot (`DepotModel`) and each place a
vehicle stops at. A vehicle starts at one depot and, unless its route is open, ends at
one (the same or another). How far and how long a drive takes comes from the vehicle's
travel profile (`TravelProfile`): pairwise driving seconds and driving meters keyed by
those ids, ``{from_id: {to_id: value}}``. A pair the dicts leave out costs the profile's
``missing_value``; a place to itself costs 0 unless given. A problem without profiles,
or a profile without one of the two dicts, drives straight lines between the
coordinates (haversine meters, at 50 km/h for the seconds). A profile may instead
drive exactly as another one does (``same_travel_as``), so profiles that differ in
something else, such as the jobs their vehicles may take, share one set of dicts.

The use cases turn the dicts into one square matrix per distinct travel before the
solve (`profile_matrices`): the engine reads matrices, the JSON stays keyed by id.
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
    "check_profile_jobs",
    "profile_job_rules",
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
    """Pairwise travel of one kind of vehicle, keyed by place id.

    Each use case extends it with the jobs a vehicle on the profile may not take
    and those it is preferred for (vehicle routing: visits, pickup-delivery:
    requests)."""

    durations: dict[str, dict[str, int]] | None = Field(
        default=None,
        description=(
            "Driving seconds, `{from_id: {to_id: seconds}}` over depot and stop ids. "
            "None drives straight lines at 50 km/h; an empty dict is refused."
        ),
    )
    distances: dict[str, dict[str, int]] | None = Field(
        default=None,
        description=(
            "Driving meters, `{from_id: {to_id: meters}}` over depot and stop ids. "
            "None drives straight lines (haversine); an empty dict is refused."
        ),
    )
    missing_value: int = Field(
        default=MISSING_TRAVEL_VALUE,
        ge=0,
        description=(
            "What a pair the two dicts leave out costs, in seconds or meters. A place to itself costs 0 unless given."
        ),
    )
    same_travel_as: str | None = Field(
        default=None,
        description=(
            "Name of another profile whose durations, distances and missing_value this one "
            "drives on, given instead of its own: profiles that differ only in the jobs their "
            "vehicles take then share one set of dicts. The named profile gives its own travel."
        ),
    )

    @model_validator(mode="after")
    def _check_travel(self):
        if self.same_travel_as is not None:
            # By value, not by the fields set: a dumped profile carries every field.
            given = [field for field in ("durations", "distances") if getattr(self, field) is not None]
            if self.missing_value != MISSING_TRAVEL_VALUE:
                given.append("missing_value")
            if given:
                raise ValueError(
                    f"same_travel_as {self.same_travel_as!r} takes the travel of that profile: "
                    f"leave out {', '.join(given)}"
                )
        for kind, pairs in (("durations", self.durations), ("distances", self.distances)):
            if pairs is not None and not pairs:
                # Every pair would cost missing_value; None is the straight-line fallback.
                raise ValueError(f"{kind} is empty: give the pairs, or leave it out for straight lines")
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
    for name, profile in profiles.items():
        source = profile.same_travel_as
        if source is None:
            continue
        if source == name:
            raise ValueError(f"profile {name!r}: same_travel_as names the profile itself")
        if source not in profiles:
            raise ValueError(f"profile {name!r}: same_travel_as names an unknown profile {source!r}")
        if (further := profiles[source].same_travel_as) is not None:
            raise ValueError(
                f"profile {name!r}: same_travel_as names {source!r}, which drives as {further!r}: name {further!r}"
            )
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


def check_profile_jobs(profile: TravelProfile, forbidden: str, preferred: str) -> None:
    """Refuse an id named twice in one of a profile's two job lists, or named in both.

    ``forbidden`` and ``preferred`` are the names of the use case's two list fields."""
    lists = {field: getattr(profile, field) for field in (forbidden, preferred)}
    for field, ids in lists.items():
        if len(set(ids)) != len(ids):
            twice = sorted({job for job in ids if ids.count(job) > 1})
            raise ValueError(f"{field} names {twice[:10]} twice")
    if both := sorted(set(lists[forbidden]) & set(lists[preferred])):
        raise ValueError(f"{both[:10]} are both in {forbidden} and in {preferred}")


def profile_job_rules(
    profiles: dict[str, TravelProfile],
    vehicles: Sequence,
    job_index: dict[str, int],
    forbidden: str,
    preferred: str,
) -> tuple[list[tuple[list[int], list[int]]], list[int]]:
    """The engine's view of the job lists: one entry per profile that has some, and an entry per vehicle.

    An entry is the profile's (forbidden, preferred) job indices, sorted, as the engine
    wants them; a vehicle's index is its profile's entry, -1 for a profile without lists.
    Only profiles some vehicle drives on get an entry. Unknown ids raise KeyError: the
    use case's input model refuses them first."""
    entries: list[tuple[list[int], list[int]]] = []
    entry_of: dict[str, int] = {}
    per_vehicle = []
    for vehicle in vehicles:
        name = vehicle.profile
        if name not in entry_of:
            profile = profiles.get(name)
            if profile is None or not (getattr(profile, forbidden) or getattr(profile, preferred)):
                entry_of[name] = -1
            else:
                entry_of[name] = len(entries)
                entries.append(
                    (
                        sorted(job_index[job] for job in getattr(profile, forbidden)),
                        sorted(job_index[job] for job in getattr(profile, preferred)),
                    )
                )
        per_vehicle.append(entry_of[name])
    return entries, per_vehicle


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
    """The engine's view of the profiles: a matrix index per profile name, and the matrices.

    One pair of matrices per distinct travel, square over ``place_ids`` (rows are "from",
    columns "to"); None for a dict the profile does not give, which the engine fills from
    the coordinates. A profile with ``same_travel_as`` takes the index of the profile it
    names, and the profiles that give neither dict share one index, on straight lines:
    they drive alike, so the engine keeps one copy. Without profiles there is one,
    `DEFAULT_PROFILE`, on straight lines.
    """
    if not profiles:
        return {DEFAULT_PROFILE: 0}, [None], [None]
    durations, distances = [], []
    index: dict[str, int] = {}
    straight = None  # the index of the profiles on straight lines, once one has it
    for name, profile in profiles.items():
        if profile.same_travel_as is not None:
            continue
        if profile.durations is None and profile.distances is None:
            if straight is None:
                straight = len(durations)
                durations.append(None)
                distances.append(None)
            index[name] = straight
            continue
        index[name] = len(durations)
        durations.append(
            None if profile.durations is None else _matrix(profile.durations, place_ids, profile.missing_value)
        )
        distances.append(
            None if profile.distances is None else _matrix(profile.distances, place_ids, profile.missing_value)
        )
    for name, profile in profiles.items():
        if profile.same_travel_as is not None:
            index[name] = index[profile.same_travel_as]
    return {name: index[name] for name in profiles}, durations, distances

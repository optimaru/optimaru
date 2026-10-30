from __future__ import annotations

from pydantic import BaseModel, ConfigDict, Field, ValidationError, model_validator


class TimeWindowModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    start: int = Field(ge=0)
    end: int = Field(gt=0)

    @model_validator(mode="after")
    def end_after_start(self) -> TimeWindowModel:
        if self.end <= self.start:
            raise ValueError("time-window end must be after start")
        return self


class MachineModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    id: str = Field(min_length=1)
    group: str = Field(min_length=1, description="Equipment group, for example LITHO or ETCH.")
    batch_capacity: int = Field(
        default=1,
        ge=1,
        description="Simultaneous compatible-lot capacity; 1 denotes a unary tool.",
    )
    minimum_batch_size: int = Field(
        default=1,
        ge=1,
        description="Preferred minimum number of compatible lots in one batch run.",
    )
    unavailable: list[TimeWindowModel] = Field(
        default_factory=list,
        description="Preventive-maintenance and other machine downtime windows.",
    )

    @model_validator(mode="after")
    def valid_batch_range(self) -> MachineModel:
        if self.minimum_batch_size > self.batch_capacity:
            raise ValueError("minimum_batch_size must not exceed batch_capacity")
        return self


class AuxiliaryResourceModel(BaseModel):
    """Reusable operator, reticle, fixture, or transport resource."""

    model_config = ConfigDict(extra="forbid")
    id: str = Field(min_length=1)
    capacity: int = Field(default=1, ge=1)
    unavailable: list[TimeWindowModel] = Field(default_factory=list)


class MachineOptionModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    machine_id: str
    duration: int = Field(gt=0)
    preference_cost: int = Field(default=0, ge=0)


class OperationModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    id: str = Field(min_length=1)
    family: str = Field(min_length=1, description="Recipe/family used for batching and setup transitions.")
    eligible_machines: list[MachineOptionModel] = Field(min_length=1)
    required_resources: list[str] = Field(
        default_factory=list,
        description="Operators, reticles, fixtures, or other auxiliary resources required while processing.",
    )
    min_queue_time: int = Field(
        default=0,
        ge=0,
        description="Minimum hold or transport wait after this operation before the next route step.",
    )
    max_queue_time: int | None = Field(
        default=None,
        ge=0,
        description="Maximum waiting time after this operation before the next route step starts.",
    )


class LotModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    id: str = Field(min_length=1)
    product: str = Field(min_length=1)
    release_time: int = Field(default=0, ge=0)
    due_time: int = Field(gt=0)
    priority: int = Field(default=1, ge=1)
    max_cycle_time: int | None = Field(
        default=None,
        gt=0,
        description="Maximum elapsed time from lot release to completion.",
    )
    operations: list[OperationModel] = Field(min_length=1)


class SetupTimeModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    machine_group: str
    from_family: str
    to_family: str
    duration: int = Field(ge=0)


class TransportTimeModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    from_group: str
    to_group: str
    duration: int = Field(ge=0)


class AssignmentModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    lot_id: str
    operation_id: str
    machine_id: str
    start: int = Field(ge=0)
    end: int = Field(gt=0)
    batch_id: str | None = None

    @model_validator(mode="after")
    def end_after_start(self) -> AssignmentModel:
        if self.end <= self.start:
            raise ValueError("assignment end must be after start")
        return self


class SemiconductorScheduleModel(BaseModel):
    """Re-entrant semiconductor-fab lot and equipment scheduling contract."""

    model_config = ConfigDict(extra="forbid")
    machines: list[MachineModel] = Field(min_length=1)
    auxiliary_resources: list[AuxiliaryResourceModel] = Field(default_factory=list)
    setup_times: list[SetupTimeModel] = Field(default_factory=list)
    transport_times: list[TransportTimeModel] = Field(
        default_factory=list,
        description="Minimum inter-operation transport time between equipment groups.",
    )
    lots: list[LotModel] = Field(min_length=1)
    pinned_assignments: list[AssignmentModel] = Field(
        default_factory=list,
        description="Frozen operation assignments retained during real-time replanning.",
    )
    assignments: list[AssignmentModel] = Field(default_factory=list)
    makespan: int | None = None
    total_weighted_tardiness: int | None = None
    total_cycle_time: int | None = None
    score_hard: int | None = None
    score_soft: float | None = None
    warm_start: bool = Field(
        default=False,
        description="Start the search from `assignments`; a complete seed is scored as supplied, not decoded.",
    )

    @model_validator(mode="after")
    def valid_references(self) -> SemiconductorScheduleModel:
        machine_by_id = {machine.id: machine for machine in self.machines}
        if len(machine_by_id) != len(self.machines):
            raise ValueError("machine ids must be unique")
        resource_ids = {resource.id for resource in self.auxiliary_resources}
        if len(resource_ids) != len(self.auxiliary_resources):
            raise ValueError("auxiliary resource ids must be unique")
        lot_ids = {lot.id for lot in self.lots}
        if len(lot_ids) != len(self.lots):
            raise ValueError("lot ids must be unique")
        operation_keys: set[tuple[str, str]] = set()
        operation_by_key: dict[tuple[str, str], OperationModel] = {}
        batch_durations: dict[tuple[str, str], int] = {}
        for lot in self.lots:
            ids = {operation.id for operation in lot.operations}
            if len(ids) != len(lot.operations):
                raise ValueError(f"operation ids must be unique inside lot {lot.id!r}")
            for operation in lot.operations:
                operation_keys.add((lot.id, operation.id))
                operation_by_key[(lot.id, operation.id)] = operation
                if unknown := set(operation.required_resources) - resource_ids:
                    raise ValueError(f"unknown required resources: {sorted(unknown)}")
                for option in operation.eligible_machines:
                    machine = machine_by_id.get(option.machine_id)
                    if machine is None:
                        raise ValueError(f"unknown eligible machine: {option.machine_id}")
                    if machine.batch_capacity > 1:
                        key = (machine.id, operation.family)
                        previous_duration = batch_durations.setdefault(key, option.duration)
                        if previous_duration != option.duration:
                            raise ValueError("operations in the same machine/family batch must have equal duration")
        assigned_keys: set[tuple[str, str]] = set()
        for assignment in self.assignments:
            key = (assignment.lot_id, assignment.operation_id)
            if key not in operation_keys:
                raise ValueError(f"assignment references unknown operation: {key}")
            if assignment.machine_id not in machine_by_id:
                raise ValueError(f"assignment references unknown machine: {assignment.machine_id}")
            if key in assigned_keys:
                raise ValueError(f"operation is assigned more than once: {key}")
            assigned_keys.add(key)
        pinned_keys = []
        for assignment in self.pinned_assignments:
            key = (assignment.lot_id, assignment.operation_id)
            if key not in operation_keys:
                raise ValueError(f"pinned assignment references unknown operation: {key}")
            if assignment.machine_id not in machine_by_id:
                raise ValueError(f"pinned assignment references unknown machine: {assignment.machine_id}")
            options = {option.machine_id: option for option in operation_by_key[key].eligible_machines}
            option = options.get(assignment.machine_id)
            if option is None or assignment.end - assignment.start != option.duration:
                raise ValueError("pinned assignments must use an eligible machine and duration")
            pinned_keys.append(key)
        if len(set(pinned_keys)) != len(pinned_keys):
            raise ValueError("an operation may only be pinned once")
        return self

    def to_core(self) -> SemiconductorScheduleModel:
        return self.model_copy(deep=True)

    @classmethod
    def from_core(cls, problem: SemiconductorScheduleModel) -> SemiconductorScheduleModel:
        return problem.model_copy(deep=True)


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is a solved problem model or a dict of the same shape; assignments are matched by lot and operation
    id. Machine eligibility and duration are checked here, against every option of the operation. Anything `solve`
    returns passes, feasible or not. Everything else, such as overlaps or route order, stays a hard penalty that the
    seed may break.

    The search state is a machine choice and a priority rank per operation, which `_construct` decodes into a
    schedule. The seed is converted to that state: the machine gives the choice, the order of the start times gives
    the rank. The decoder left-shifts operations and re-derives batches, so decoding a seed can give another schedule,
    worse as well as better, even when the seed came out of a solve.

    Guaranteed for a complete seed, one that assigns every operation: `solve` scores the supplied assignments directly
    without decoding them, returns them unchanged, batch ids included, unless the search finds a strictly better
    schedule, and so never returns a score worse than the seed's.

    Not guaranteed: that the search stays close to the seed or can improve it. Every schedule it visits after the seed
    is decoded, and a seed the decoder cannot reproduce may have no accepted neighbour, in which case it comes back
    as supplied. A partial seed cannot be scored, so it is decoded from the start: it keeps its machines and relative
    order, the left-out operations rank last on their default machine, and all start times are recomputed.
    """
    if isinstance(solution, BaseModel):
        solution = solution.model_dump(mode="json")
    if not isinstance(solution, dict) or not isinstance(solution.get("assignments"), list):
        raise ValueError("solution must be a problem model or a dict with an 'assignments' list")
    operations = {(lot.id, operation.id): operation for lot in problem.lots for operation in lot.operations}
    seeded_by_key: dict[tuple[str, str], AssignmentModel] = {}
    for entry in solution["assignments"]:
        try:
            assignment = AssignmentModel.model_validate(entry)
        except ValidationError as error:
            raise ValueError(f"invalid assignment {entry!r}: {error}") from error
        key = (assignment.lot_id, assignment.operation_id)
        if key not in operations:
            raise ValueError(f"solution references unknown operation: {key}")
        if key in seeded_by_key:
            raise ValueError(f"solution assigns operation {key} more than once")
        durations = [
            option.duration
            for option in operations[key].eligible_machines
            if option.machine_id == assignment.machine_id
        ]
        if not durations:
            raise ValueError(f"operation {key} cannot run on machine {assignment.machine_id!r}")
        if assignment.end - assignment.start not in durations:
            raise ValueError(
                f"operation {key} takes {' or '.join(map(str, durations))} on machine {assignment.machine_id!r}, "
                f"not {assignment.end - assignment.start}"
            )
        seeded_by_key[key] = assignment
    seeded = problem.model_copy(deep=True)
    seeded.assignments = list(seeded_by_key.values())
    seeded.makespan = seeded.total_weighted_tardiness = seeded.total_cycle_time = None
    seeded.score_hard = seeded.score_soft = None
    seeded.warm_start = True
    return seeded

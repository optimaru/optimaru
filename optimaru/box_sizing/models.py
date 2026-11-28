"""Pydantic JSON contract for the box-sizing domain.

The canonical problem as JSON. The engine holds a parcel's box as a position in
`boxes`; the model names it by the box's id::

    from optimaru.box_sizing.models import BoxSizingProblemModel

    model = BoxSizingProblemModel.model_validate(data)        # JSON -> pydantic
    solved = bs.solve(model, time_limit_seconds=5.0)          # a new model, with the outputs
    print(solved.box_types_used, solved.void_volume, solved.fill_ratio)

Lengths and weights are whole numbers in the caller's unit (the guide's examples use mm
and g). The canonical problem carries no constraint weights (``extra="forbid"``): weights
come only from constraint selections, and the defaults are the C++ components'
(``box_sizing.constraint_info()``).
"""

from __future__ import annotations

from pydantic import Field, ValidationInfo, model_validator

from .._constraint import ParameterUse
from .._models import SolveRequest, StrictModel, entities_by_id, solution_dict, solve_model_request
from . import _core

__all__ = [
    "BoxModel",
    "BoxSizingProblemModel",
    "BoxSizingRequest",
    "ParcelModel",
    "parameter_uses",
    "solve_request",
    "with_initial_solution",
]

#: The largest side and weight the engine takes (ScoreCalculator::kMaxSide, kMaxWeight).
MAX_SIDE = 2_000_000
MAX_WEIGHT = 1_000_000_000_000


class BoxModel(StrictModel):
    """One candidate box size."""

    id: str = Field(min_length=1, description="Unique box identifier.")
    length: int = Field(gt=0, le=MAX_SIDE, description="Inner length.")
    width: int = Field(gt=0, le=MAX_SIDE, description="Inner width.")
    height: int = Field(gt=0, le=MAX_SIDE, description="Inner height.")
    max_weight: int | None = Field(
        default=None, ge=0, le=MAX_WEIGHT, description="Heaviest parcel the box carries; None = no limit."
    )
    cost: float = Field(
        default=0.0, ge=0, allow_inf_nan=False, description="What one box of this size costs (material, freight)."
    )
    required: bool = Field(
        default=False,
        description="Already stocked and staying: it counts as in use even when no parcel takes it.",
    )
    parcels: int | None = Field(
        default=None, ge=0, description="Output: how many parcels (quantities summed) ship in this box."
    )


class ParcelModel(StrictModel):
    """The packed content of one shipment, by its bounding size. It fits a box when its three
    sides, sorted, are each at most the box's sorted sides (any rotation)."""

    id: str = Field(min_length=1, description="Unique parcel identifier.")
    length: int = Field(gt=0, le=MAX_SIDE)
    width: int = Field(gt=0, le=MAX_SIDE)
    height: int = Field(gt=0, le=MAX_SIDE)
    weight: int = Field(default=0, ge=0, le=MAX_WEIGHT, description="Weight of one shipment.")
    quantity: int = Field(default=1, ge=1, description="How many shipments had this size.")
    box: str | None = Field(default=None, description="Id of the box it ships in; None = not decided.")


def _sides(thing) -> tuple[int, int, int]:
    """The three sides, ascending: a parcel may be turned any way."""
    low, middle, high = sorted((thing.length, thing.width, thing.height))
    return low, middle, high


def _parcels_no_box_takes(boxes: list[BoxModel], parcels: list[ParcelModel]) -> list[ParcelModel]:
    """The parcels that fit none of `boxes`, by size in some rotation and by weight (the
    engine's BoxSizingProblem::fits).

    The boxes are tried largest first, so a parcel that fits is found at the first or second
    box and the scan is one pass over the parcels in practice; only a parcel that fits none
    is tried against every box."""
    candidates = sorted(
        ((*_sides(box), box.max_weight) for box in boxes),
        key=lambda sides: sides[0] * sides[1] * sides[2],
        reverse=True,
    )
    left_over = []
    for parcel in parcels:
        low, middle, high = _sides(parcel)
        weight = parcel.weight
        for box_low, box_middle, box_high, max_weight in candidates:
            if low <= box_low and middle <= box_middle and high <= box_high:
                if max_weight is None or weight <= max_weight:
                    break
        else:
            left_over.append(parcel)
    return left_over


class BoxSizingProblemModel(StrictModel):
    """Which box sizes to stock: every parcel ships in one of the candidate boxes, at most
    `max_box_types` distinct boxes are in use, and the least air is shipped."""

    boxes: list[BoxModel] = Field(min_length=1, description="The candidate box sizes.")
    parcels: list[ParcelModel] = Field(description="The parcels shipped, one entry per size.")
    max_box_types: int | None = Field(
        default=None, ge=1, description="How many distinct boxes may be in use (required ones included)."
    )
    warm_start: bool = Field(
        default=False,
        description=(
            "Start the search from the parcels' `box`; parcels without one are placed around them by the "
            "construction. False: a solve ignores the boxes on the parcels."
        ),
    )
    score_hard: int | None = Field(
        default=None, description="Output: negative weighted hard score (fit, weight, box type limit)."
    )
    score_soft: float | None = Field(
        default=None, description="Output: negative weighted soft score (void volume, box cost)."
    )
    box_types_used: int | None = Field(
        default=None, ge=0, description="Output: distinct boxes in use, the required ones included."
    )
    void_volume: int | None = Field(
        default=None, ge=0, description="Output: sum over the parcels of quantity * (box volume - parcel volume)."
    )
    fill_ratio: float | None = Field(
        default=None, ge=0, description="Output: shipped parcel volume / shipped box volume."
    )

    @model_validator(mode="after")
    def _validate(self, info: ValidationInfo) -> BoxSizingProblemModel:
        # `from_core` reads a problem the engine has just checked: it passes this context so
        # the scan over the parcels is not made a second time on the way out of a solve.
        return self._check(fits=not (info.context or {}).get("checked_by_the_engine", False))

    def _check(self, *, fits: bool = True) -> BoxSizingProblemModel:
        """Ids are unique, a parcel's box is a candidate, and the problem can be feasible at all:
        every parcel fits some candidate (skipped with ``fits=False``), and the required boxes
        fit under the limit."""
        box_ids = [box.id for box in self.boxes]
        if len(box_ids) != len(set(box_ids)):
            raise ValueError("box ids must be unique")
        parcel_ids = [parcel.id for parcel in self.parcels]
        if len(parcel_ids) != len(set(parcel_ids)):
            raise ValueError("parcel ids must be unique")
        known = set(box_ids)
        for parcel in self.parcels:
            if parcel.box is not None and parcel.box not in known:
                raise ValueError(f"parcel {parcel.id!r}: box {parcel.box!r} is not one of the candidate boxes")
        for parcel in _parcels_no_box_takes(self.boxes, self.parcels) if fits else ():
            raise ValueError(
                f"parcel {parcel.id!r} ({parcel.length} x {parcel.width} x {parcel.height}, weight "
                f"{parcel.weight}) fits none of the candidate boxes: add a box that takes it, or leave it out"
            )
        required = sum(box.required for box in self.boxes)
        if self.max_box_types is not None and required > self.max_box_types:
            raise ValueError(f"max_box_types is {self.max_box_types}, but {required} boxes are required")
        return self

    def to_core(self) -> _core.BoxSizingProblem:
        """pydantic -> engine object (the output fields are ignored; the engine recomputes them)."""
        # The fields stay mutable after validation. The engine checks the fit itself when it
        # solves (ScoreCalculator::requireSolvable), with the same message.
        self._check(fits=False)
        position = {box.id: index for index, box in enumerate(self.boxes)}
        return _core.BoxSizingProblem(
            boxes=[
                _core.Box(box.id, box.length, box.width, box.height, box.max_weight, box.cost, box.required)
                for box in self.boxes
            ],
            parcels=[
                _core.Parcel(
                    parcel.id,
                    parcel.length,
                    parcel.width,
                    parcel.height,
                    parcel.weight,
                    parcel.quantity,
                    -1 if parcel.box is None else position[parcel.box],
                )
                for parcel in self.parcels
            ],
            max_box_types=self.max_box_types,
            warm_start=self.warm_start,
        )

    @classmethod
    def from_core(cls, problem: _core.BoxSizingProblem) -> BoxSizingProblemModel:
        """engine object -> pydantic, including the boxes of the parcels, the score and the derived outputs."""
        boxes = problem.boxes
        shipped = problem.parcels_per_box()
        data = {
            "boxes": [
                BoxModel(
                    id=box.id,
                    length=box.length,
                    width=box.width,
                    height=box.height,
                    max_weight=box.max_weight,
                    cost=box.cost,
                    required=box.required,
                    parcels=shipped[index],
                )
                for index, box in enumerate(boxes)
            ],
            "parcels": [
                ParcelModel(
                    id=parcel.id,
                    length=parcel.length,
                    width=parcel.width,
                    height=parcel.height,
                    weight=parcel.weight,
                    quantity=parcel.quantity,
                    box=None if parcel.box < 0 else boxes[parcel.box].id,
                )
                for parcel in problem.parcels
            ],
            "max_box_types": problem.max_box_types,
            "warm_start": problem.warm_start,
            "score_hard": problem.score.hard,
            "score_soft": problem.score.soft,
            "box_types_used": problem.box_types_used(),
            "void_volume": problem.void_volume(),
            "fill_ratio": problem.fill_ratio(),
        }
        # A problem the engine has solved or scored: the fit of every parcel is its to check.
        return cls.model_validate(data, context={"checked_by_the_engine": True})


def parameter_uses(problem: BoxSizingProblemModel) -> list[ParameterUse]:
    """The parameters `problem` sets away from their defaults that act only through a cost.

    `solve` refuses a model whose selection holds none of a parameter's costs
    (`optimaru._constraint.require_costs`); the table is "Parameters and their costs" in
    docs/use-cases/README.md. The sizes and the quantities are required, so never "set"."""
    uses = []
    for box in problem.boxes:
        where = f"box {box.id}"
        if box.max_weight is not None:
            uses.append(ParameterUse(where, "max_weight", None, ("box_weight_cost",)))
        if box.cost:
            uses.append(ParameterUse(where, "cost", 0.0, ("box_cost",)))
        if box.required:
            uses.append(ParameterUse(where, "required", False, ("box_type_limit_cost", "box_type_cost")))
    if problem.max_box_types is not None:
        uses.append(ParameterUse("the problem", "max_box_types", None, ("box_type_limit_cost",)))
    return uses


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is a solved problem (what `solve` returned for a model), or a plain dict of
    that shape (the JSON the API returned). Parcels are matched by id, and a parcel's `box`
    is a box id, so the solution may list the boxes in another order. A parcel the solution
    leaves out, or holds without a box, is placed around the others by the construction, and
    only it, so a complete solution starts the search exactly from it. `problem` is not
    mutated; the copy has `warm_start` set.

    Raises ValueError naming the parcel for an unknown or repeated id and for a `box` that
    is not one of the problem's candidate boxes.
    """
    if not isinstance(problem, BoxSizingProblemModel):
        raise TypeError(f"problem must be a BoxSizingProblemModel: {type(problem)!r}")
    solved = solution_dict(solution)
    parcels = entities_by_id(solved, "parcels", [parcel.id for parcel in problem.parcels])
    known = {box.id for box in problem.boxes}
    seeded = problem.model_copy(deep=True)
    seeded.score_hard = seeded.score_soft = None
    seeded.box_types_used = seeded.void_volume = seeded.fill_ratio = None
    for box in seeded.boxes:
        box.parcels = None
    for parcel in seeded.parcels:
        box = parcels.get(parcel.id, {}).get("box")
        if box is not None and (not isinstance(box, str) or box not in known):
            raise ValueError(f"parcel {parcel.id!r}: box {box!r} is not one of the candidate boxes")
        parcel.box = box
    seeded.warm_start = True
    return seeded


class BoxSizingRequest(SolveRequest):
    """The box sizing problem with the settings of one solve (see `SolveRequest` for those)."""

    model_input: BoxSizingProblemModel


def solve_request(request: BoxSizingRequest, *, constraints=None) -> BoxSizingProblemModel:
    """Solve a pydantic request and return the solved problem model.

    This is ``solve(request.model_input, ...)`` with the request's settings as the keywords
    of the same names (see `SolveRequest`), and ``constraints`` the selection ``solve``
    takes. The request is not changed.
    """
    from . import SolverConfig, solve

    return solve_model_request(solve, SolverConfig, request, constraints=constraints)

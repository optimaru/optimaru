from .common import Constraint, score_costs


class Capacity(Constraint):
    """Positive amount by which selected item weight exceeds capacity."""

    name = "capacity"
    label = "Capacity"
    default_hard = True


class ResourceCapacities(Constraint):
    """Overflow across additional resource dimensions."""

    name = "resource_capacities"
    label = "Resource capacities"
    default_hard = True


class MandatoryItems(Constraint):
    """Number of mandatory items that are not selected."""

    name = "mandatory_items"
    label = "Mandatory items"
    default_hard = True


class ForbiddenItems(Constraint):
    """Number of forbidden items that are selected."""

    name = "forbidden_items"
    label = "Forbidden items"
    default_hard = True


class SelectionGroupBounds(Constraint):
    """Selection count outside each group's inclusive lower and upper bounds."""

    name = "selection_group_bounds"
    label = "Selection group bounds"
    default_hard = True


class ItemDependencies(Constraint):
    """Selected items whose required companion item is not selected."""

    name = "item_dependencies"
    label = "Item dependencies"
    default_hard = True


class MaximizeValue(Constraint):
    """Negative selected value, so maximizing score maximizes value."""

    name = "maximize_value"
    label = "Maximize value"


capacity = Capacity()
resource_capacities = ResourceCapacities()
mandatory_items = MandatoryItems()
forbidden_items = ForbiddenItems()
selection_group_bounds = SelectionGroupBounds()
item_dependencies = ItemDependencies()
maximize_value = MaximizeValue()
DEFAULTS = [
    capacity,
    resource_capacities,
    mandatory_items,
    forbidden_items,
    selection_group_bounds,
    item_dependencies,
    maximize_value,
]


def score(costs: dict, constraints=None):
    return score_costs(costs, constraints, DEFAULTS)

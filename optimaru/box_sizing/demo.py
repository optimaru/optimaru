"""The demo instance: a small warehouse's shipments and the box sizes it could stock.

24 candidate box sizes (inner sizes in mm, carrying up to 20 or 30 kg) and, by default, 120
parcel sizes drawn around a handful of product shapes, with a few sizes shipped often and
most shipped rarely, from a seeded ``random.Random``. The shelf has room for six sizes.
"""

import random

from .models import BoxModel, BoxSizingProblemModel, ParcelModel

__all__ = ["generate_demo_data"]

# Inner length, width, height in mm.
CANDIDATE_SIZES = (
    (160, 110, 60),
    (180, 130, 90),
    (200, 150, 100),
    (220, 160, 60),
    (230, 180, 120),
    (250, 200, 100),
    (260, 180, 150),
    (270, 210, 150),
    (300, 200, 120),
    (300, 230, 180),
    (320, 240, 100),
    (330, 250, 150),
    (340, 250, 210),
    (350, 300, 150),
    (380, 280, 230),
    (400, 300, 200),
    (410, 310, 280),
    (430, 320, 180),
    (450, 350, 250),
    (480, 380, 340),
    (500, 400, 300),
    (520, 380, 260),
    (550, 420, 350),
    (600, 450, 400),
)

# What the warehouse ships: (length, width, height) of a typical packed order, in mm.
PRODUCT_SHAPES = (
    (150, 100, 40),
    (190, 140, 80),
    (240, 170, 60),
    (260, 200, 130),
    (310, 220, 90),
    (330, 240, 190),
    (390, 290, 170),
    (440, 330, 230),
    (520, 390, 310),
)


def generate_demo_data(seed: int = 0, parcel_count: int = 120, max_box_types: int | None = 6) -> BoxSizingProblemModel:
    """Build the demo `BoxSizingProblemModel` (every parcel's `box` None: the unsolved state).

    `parcel_count` parcel sizes; `max_box_types` sizes may be stocked (None: no limit).
    The same arguments give the same instance."""
    rng = random.Random(seed)
    boxes = [
        BoxModel(
            id=f"BOX-{index + 1:02d}",
            length=length,
            width=width,
            height=height,
            max_weight=20_000 if length * width * height < 20_000_000 else 30_000,
        )
        for index, (length, width, height) in enumerate(CANDIDATE_SIZES)
    ]
    largest = CANDIDATE_SIZES[-1]
    parcels = []
    for index in range(parcel_count):
        # Small orders are the common ones.
        shape = PRODUCT_SHAPES[min(int(rng.expovariate(0.45)), len(PRODUCT_SHAPES) - 1)]
        sides = [
            min(limit, max(20, round(side * rng.uniform(0.75, 1.2) / 5) * 5)) for side, limit in zip(shape, largest)
        ]
        volume_liters = sides[0] * sides[1] * sides[2] / 1_000_000
        parcels.append(
            ParcelModel(
                id=f"P{index + 1:03d}",
                length=sides[0],
                width=sides[1],
                height=sides[2],
                weight=min(19_000, max(100, round(volume_liters * rng.uniform(120, 420) / 10) * 10)),
                # A few sizes ship often, most a handful of times.
                quantity=max(1, round(rng.paretovariate(1.3))),
            )
        )
    return BoxSizingProblemModel(boxes=boxes, parcels=parcels, max_box_types=max_box_types)

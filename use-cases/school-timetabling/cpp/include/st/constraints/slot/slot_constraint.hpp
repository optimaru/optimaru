#ifndef ST_CONSTRAINTS_SLOT_SLOT_CONSTRAINT_HPP
#define ST_CONSTRAINTS_SLOT_SLOT_CONSTRAINT_HPP

#include "st/constraints/common.hpp"

namespace st::canon
{

// ---------------------------------------------------------------------------
// Per-slot unit constraints (exact deltaCost — the calculator never re-costs
// a whole slot on the move path)
// ---------------------------------------------------------------------------

struct SlotDeltaGroup;

// Marker base of the per-slot family (classConflictCost, teacherConflictCost,
// disallowedSlotCost, roomCapacityCost, avoidedPeriodCost,
// nonPreferredPeriodCost): their marginal costs are all projections of one
// lesson joining/leaving a slot, so the calculator compiles every present
// member into ONE SlotDeltaGroup evaluate per member change — dense
// per-lesson tables plus the monolithic (owner, slot) count maps replace the
// six per-constraint deltaCost calls (which re-scan the slot's members).
// Metadata (weight / level / exponent) stays calculator-owned, so the knobs
// keep working unchanged.
struct GroupableSlotConstraint : CanonUnitConstraint
{
    using CanonUnitConstraint::CanonUnitConstraint;
    const char *deltaGroupKey() const override;
    std::unique_ptr<optimaru::UnitConstraintDeltaGroup<Timetable>>
    createDeltaGroup() const override;
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_SLOT_SLOT_CONSTRAINT_HPP

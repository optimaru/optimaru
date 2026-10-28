#ifndef ST_CONSTRAINTS_GLOBAL_GLOBAL_CONSTRAINT_HPP
#define ST_CONSTRAINTS_GLOBAL_GLOBAL_CONSTRAINT_HPP

#include "st/constraints/common.hpp"

namespace st::canon
{

// ---------------------------------------------------------------------------
// Cross-slot global constraints (BASIC member-change fast path: the
// calculator delivers every lesson join/leave, including the inverse writes
// of a rolled-back move, so exactly reversible aggregates stay consistent)
// ---------------------------------------------------------------------------

// Shared base: supportsMemberChange() routes everything through
// onMemberChanged; the unit-diff hook is unreachable for BASIC solutions.
struct MemberChangeGlobalConstraint : CanonGlobalConstraint
{
    using CanonGlobalConstraint::CanonGlobalConstraint;
    bool supportsMemberChange() const override { return true; }
    void onUnitChanged(const Timetable & /*table*/,
                       int /*unit*/,
                       const std::vector<int> & /*members*/) override
    {
        throw std::logic_error(name
                               + ": onUnitChanged is unreachable "
                                 "(member-change fast path)");
    }
};

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_GLOBAL_GLOBAL_CONSTRAINT_HPP

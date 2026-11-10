#ifndef ES_CONSTRAINTS_SCOPED_UNIT_HPP
#define ES_CONSTRAINTS_SCOPED_UNIT_HPP

#include "es/constraints/common.hpp"

namespace es {

// Rule scope: restrict any unit constraint to a subset of workers and/or
// shifts. The caller resolves worker/shift ids or tags to engine indices before
// solving; this wrapper honours them. Orthogonal to each constraint's logic —
// no per-constraint code changes needed.
//
//   worker scope: the constraint is skipped entirely for out-of-scope workers
//                 (their cost contribution is 0).
//   shift scope:  only in-scope shifts are counted (members are filtered
//                 before the inner cost runs).
//
// Empty index list => "all" for that dimension. Indices are resolved against
// the solution order by the caller; on_reset expands them to constant-time masks.
//
// The inner constraint keeps its own cost/delta_cost. Scoped constraints opt
// out of the calculator's structural delta-grouping and shared-context fast
// paths (those virtuals stay at their nullptr defaults here), which is correct:
// grouping keys off the concrete type (a ScopedUnit is not RequiredSkillCost
// etc.), and inner constraints that use a shared context (e.g. DayPatternCost)
// fall back to their own local context when none is bound.
//
// The wrapper takes over ALL of the inner component's metadata — registry
// name, weight, level, reward, level_flippable, exponent, label — so a scoped
// reward stays a reward (hard() refused) and the knobs a caller re-states
// afterwards (python::apply_knobs on the ScopedUnit) see the inner defaults.
// The inner component's own knobs are never read again: the calculator
// weighs the wrapper.
struct ScopedUnit final : EsUnitConstraint {
    std::unique_ptr<EsUnitConstraint> inner_;
    bool hasWorker_;
    bool hasShift_;
    std::vector<int> workerIdx_;
    std::vector<int> shiftIdx_;
    std::vector<std::uint8_t> workerMask_;
    std::vector<std::uint8_t> shiftMask_;

    ScopedUnit(std::unique_ptr<EsUnitConstraint> inner, std::vector<int> workerIdx,
               std::vector<int> shiftIdx, bool hasWorker, bool hasShift)
        : EsUnitConstraint(inner->name, inner->weight, inner->level),
          inner_(std::move(inner)),
          hasWorker_(hasWorker),
          hasShift_(hasShift),
          workerIdx_(std::move(workerIdx)),
          shiftIdx_(std::move(shiftIdx)) {
        static_cast<optimaru::ConstraintMetadata&>(*this) = *inner_;
    }

    void on_reset(const EmployeeSchedule& s) override {
        inner_->on_reset(s);
        if (hasWorker_) {
            workerMask_.assign(s.employees.size(), 0);
            for (int i : workerIdx_)
                if (i >= 0 && i < static_cast<int>(s.employees.size())) workerMask_[i] = 1;
        }
        if (hasShift_) {
            shiftMask_.assign(s.shifts.size(), 0);
            for (int i : shiftIdx_)
                if (i >= 0 && i < static_cast<int>(s.shifts.size())) shiftMask_[i] = 1;
        }
    }

    double cost(const EmployeeSchedule& s, int unit,
                const std::vector<int>& members) const override {
        if (hasWorker_ && !workerMask_[unit]) return 0.0;
        if (!hasShift_) return inner_->cost(s, unit, members);
        std::vector<int> filtered;
        filtered.reserve(members.size());
        for (int i : members)
            if (shiftMask_[i]) filtered.push_back(i);
        return inner_->cost(s, unit, filtered);
    }

    // A worker-scoped constraint can only be non-zero for the workers it lists
    // (cost() returns 0 for every other unit), so expose that scope: the
    // calculator then skips it when re-costing all the workers it can't touch.
    // Without a worker scope it may apply to any unit -> nullptr ("all").
    const std::vector<int>* scoped_units() const override {
        return hasWorker_ ? &workerIdx_ : nullptr;
    }

    // Worker scope preserves the inner delta contract; shift scope filters
    // members, which breaks delta exactness, so fall back to full recompute.
    bool supports_delta() const override { return !hasShift_ && inner_->supports_delta(); }
    double delta_cost(const EmployeeSchedule& s, int unit, const std::vector<int>& members,
                      int entity) const override {
        if (hasWorker_ && !workerMask_[unit]) return 0.0;
        return inner_->delta_cost(s, unit, members, entity);
    }
};

}  // namespace es

#endif  // ES_CONSTRAINTS_SCOPED_UNIT_HPP

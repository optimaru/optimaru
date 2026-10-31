#ifndef ES_CONSTRAINTS_ASSIGNMENT_DELTA_GROUP_HPP
#define ES_CONSTRAINTS_ASSIGNMENT_DELTA_GROUP_HPP

// The compiled delta evaluator of the assignment family. It names every
// member by type, so it has to come AFTER them — the family base they all
// derive from lives in assignment_constraint.hpp instead, which is what the
// members include.

#include "es/constraints/assignment_constraint.hpp"
#include "es/constraints/desired_day.hpp"
#include "es/constraints/desired_shift.hpp"
#include "es/constraints/max_one_shift_per_day.hpp"
#include "es/constraints/minimum_rest.hpp"
#include "es/constraints/overlapping_shift.hpp"
#include "es/constraints/required_skill.hpp"
#include "es/constraints/unavailable_employee.hpp"
#include "es/constraints/undesired_day.hpp"

namespace es {

// Compiled marginal-cost evaluator for every groupable employee-assignment
// constraint present in an assembly. Direct per-(employee, shift) terms are
// loaded from their prepared tables; all pairwise terms share one walk over
// the employee's other shifts. The core applies each raw delta through the
// constraint's independent weight/level/exponent metadata afterwards.
struct EmployeeAssignmentDeltaGroup final
    : optimaru::UnitConstraintDeltaGroup<EmployeeSchedule> {
    static constexpr std::size_t NONE = std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t PAIR_CACHE_BUDGET_BYTES = 64U * 1024U * 1024U;

    struct PairProjection {
        std::int32_t overlap = 0;
        std::int32_t gap = -1;
    };

    void add(std::size_t index, const EsUnitConstraint& constraint) override {
        if (const auto* typed = dynamic_cast<const RequiredSkill*>(&constraint)) {
            if (requiredSkill_ != nullptr) duplicate_ = true;
            requiredSkill_ = typed;
            requiredSkillIndex_ = index;
        } else if (const auto* typed = dynamic_cast<const UnavailableEmployee*>(&constraint)) {
            if (unavailable_ != nullptr) duplicate_ = true;
            unavailable_ = typed;
            unavailableIndex_ = index;
        } else if (const auto* typed = dynamic_cast<const UndesiredDay*>(&constraint)) {
            if (undesiredDay_ != nullptr) duplicate_ = true;
            undesiredDay_ = typed;
            undesiredDayIndex_ = index;
        } else if (const auto* typed = dynamic_cast<const DesiredDay*>(&constraint)) {
            if (desiredDay_ != nullptr) duplicate_ = true;
            desiredDay_ = typed;
            desiredDayIndex_ = index;
        } else if (const auto* typed = dynamic_cast<const DesiredShift*>(&constraint)) {
            if (desiredShift_ != nullptr) duplicate_ = true;
            desiredShift_ = typed;
            desiredShiftIndex_ = index;
        } else if (dynamic_cast<const OverlappingShift*>(&constraint) != nullptr) {
            if (overlapIndex_ != NONE) duplicate_ = true;
            overlapIndex_ = index;
        } else if (dynamic_cast<const MaxOneShiftPerDay*>(&constraint) != nullptr) {
            if (sameDayIndex_ != NONE) duplicate_ = true;
            sameDayIndex_ = index;
        } else if (const auto* typed = dynamic_cast<const MinimumRest*>(&constraint)) {
            if (minimumRest_ != nullptr) duplicate_ = true;
            minimumRest_ = typed;
            minimumRestIndex_ = index;
        } else {
            unsupported_ = true;
        }
        indexes_.push_back(index);
    }

    const std::vector<std::size_t>& indexes() const override { return indexes_; }
    bool hasIntegralRawDeltas() const override { return true; }

    bool isBeneficial() const override {
        if (duplicate_ || unsupported_) return false;
        const int pairConstraintCount = (overlapIndex_ != NONE ? 1 : 0) +
                                        (sameDayIndex_ != NONE ? 1 : 0) +
                                        (minimumRest_ != nullptr ? 1 : 0);
        return pairConstraintCount >= 2;
    }

    void onReset(const EmployeeSchedule& schedule) override {
        const int newShiftCount = static_cast<int>(schedule.shifts.size());
        const std::size_t n = schedule.shifts.size();
        const bool cellsFit = n == 0 || n <= std::numeric_limits<std::size_t>::max() / n;
        const std::size_t cells = cellsFit ? n * n : 0;
        const bool canCache =
                cellsFit && cells <= PAIR_CACHE_BUDGET_BYTES / sizeof(PairProjection);
        bool projectionUnchanged = pairCached_ && canCache && start_.size() == n;
        if (projectionUnchanged) {
            for (std::size_t i = 0; i < n; ++i) {
                if (start_[i] != static_cast<std::int32_t>(schedule.shifts[i].start) ||
                    end_[i] != static_cast<std::int32_t>(schedule.shifts[i].end) ||
                    day_[i] != static_cast<std::int32_t>(schedule.shifts[i].startDay)) {
                    projectionUnchanged = false;
                    break;
                }
            }
        }
        shiftCount_ = newShiftCount;
        if (projectionUnchanged) return;

        start_.resize(schedule.shifts.size());
        end_.resize(schedule.shifts.size());
        day_.resize(schedule.shifts.size());
        for (std::size_t i = 0; i < schedule.shifts.size(); ++i) {
            start_[i] = static_cast<std::int32_t>(schedule.shifts[i].start);
            end_[i] = static_cast<std::int32_t>(schedule.shifts[i].end);
            day_[i] = static_cast<std::int32_t>(schedule.shifts[i].startDay);
        }

        pairCached_ = canCache;
        if (!pairCached_) {
            std::vector<PairProjection>().swap(pairProjection_);
            return;
        }
        pairProjection_.assign(cells, {});
        for (int a = 0; a < shiftCount_; ++a) {
            for (int b = a + 1; b < shiftCount_; ++b) {
                PairProjection& ab = pairProjection_[pairOffset(a, b)];
                PairProjection& ba = pairProjection_[pairOffset(b, a)];
                ab.overlap = computeOverlap(a, b);
                ba.overlap = ab.overlap;
                ab.gap = computeGap(a, b);
                ba.gap = computeGap(b, a);
            }
        }
    }

    void evaluate(const EmployeeSchedule& schedule, int unit, const std::vector<int>& members,
                  int entity, double* deltas) override {
        if (requiredSkill_ != nullptr)
            deltas[requiredSkillIndex_] = requiredSkill_->memberDelta(schedule, unit, entity);
        if (unavailable_ != nullptr)
            deltas[unavailableIndex_] = unavailable_->memberDelta(schedule, unit, entity);
        if (undesiredDay_ != nullptr)
            deltas[undesiredDayIndex_] = undesiredDay_->memberDelta(schedule, unit, entity);
        if (desiredDay_ != nullptr)
            deltas[desiredDayIndex_] = desiredDay_->memberDelta(schedule, unit, entity);
        if (desiredShift_ != nullptr)
            deltas[desiredShiftIndex_] = desiredShift_->memberDelta(unit, entity);

        std::int64_t overlapTotal = 0;
        std::int64_t sameDayTotal = 0;
        std::int64_t restTotal = 0;
        const bool needsOverlap = overlapIndex_ != NONE;
        const bool needsSameDay = sameDayIndex_ != NONE;
        const bool needsRest = minimumRest_ != nullptr;
        const std::int32_t minGap = needsRest
                                            ? static_cast<std::int32_t>(
                                                      minimumRest_->minGapMinutes())
                                            : 0;
        const std::int32_t entityDay = day_[entity];
        for (int other : members) {
            if (other == entity) continue;
            std::int32_t overlap;
            std::int32_t gap;
            std::uint8_t sameDay;
            if (pairCached_) {
                const PairProjection& pair = pairProjection_[pairOffset(entity, other)];
                overlap = pair.overlap;
                gap = pair.gap;
                sameDay = day_[other] == entityDay ? 1 : 0;
            } else {
                overlap = computeOverlap(entity, other);
                gap = computeGap(entity, other);
                sameDay = day_[other] == entityDay ? 1 : 0;
            }
            if (needsOverlap) overlapTotal += overlap;
            if (needsSameDay) sameDayTotal += sameDay;
            if (needsRest && gap >= 0 && gap < minGap) {
                restTotal += minGap - gap;
            }
        }

        if (needsOverlap) deltas[overlapIndex_] = static_cast<double>(overlapTotal);
        if (needsSameDay) deltas[sameDayIndex_] = static_cast<double>(sameDayTotal);
        if (needsRest) deltas[minimumRestIndex_] = static_cast<double>(restTotal);
    }

private:
    std::size_t pairOffset(int a, int b) const {
        return static_cast<std::size_t>(a) * shiftCount_ + b;
    }

    std::int32_t computeOverlap(int x, int y) const {
        const std::int32_t overlap = std::min(end_[x], end_[y]) - std::max(start_[x], start_[y]);
        return overlap > 0 ? overlap : 0;
    }

    std::int32_t computeGap(int x, int y) const {
        const int early = (start_[x] <= start_[y]) ? x : y;
        const int late = (early == x) ? y : x;
        return end_[early] > start_[late] ? -1 : start_[late] - end_[early];
    }

    std::vector<std::size_t> indexes_;
    const RequiredSkill* requiredSkill_ = nullptr;
    const UnavailableEmployee* unavailable_ = nullptr;
    const UndesiredDay* undesiredDay_ = nullptr;
    const DesiredDay* desiredDay_ = nullptr;
    const DesiredShift* desiredShift_ = nullptr;
    const MinimumRest* minimumRest_ = nullptr;
    std::size_t requiredSkillIndex_ = NONE;
    std::size_t unavailableIndex_ = NONE;
    std::size_t undesiredDayIndex_ = NONE;
    std::size_t desiredDayIndex_ = NONE;
    std::size_t desiredShiftIndex_ = NONE;
    std::size_t overlapIndex_ = NONE;
    std::size_t sameDayIndex_ = NONE;
    std::size_t minimumRestIndex_ = NONE;
    bool duplicate_ = false;
    bool unsupported_ = false;
    bool pairCached_ = false;
    int shiftCount_ = 0;
    std::vector<std::int32_t> start_, end_, day_;
    std::vector<PairProjection> pairProjection_;
};

inline const char* GroupableAssignmentConstraint::deltaGroupKey() const {
    return "es.employee-assignment-delta.v1";
}

inline std::unique_ptr<optimaru::UnitConstraintDeltaGroup<EmployeeSchedule>>
GroupableAssignmentConstraint::createDeltaGroup() const {
    return std::make_unique<EmployeeAssignmentDeltaGroup>();
}

}  // namespace es

#endif  // ES_CONSTRAINTS_ASSIGNMENT_DELTA_GROUP_HPP

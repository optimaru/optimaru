#ifndef ES_INCREMENTAL_CALCULATOR_HPP
#define ES_INCREMENTAL_CALCULATOR_HPP

#include <cstddef>
#include <vector>

#include "es/domain.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace es {

// The employee-scheduling implementation of the engine's
// optimaru::IncrementalScoreCalculator.
//
// The score decomposes per employee: every pairwise constraint is within a
// single employee, and per-shift constraints belong to that shift's employee.
// The only cross-employee term is the load-balancing "unfairness", a closed-form
// function of the per-employee shift counts.
//
// beforeVariableChanged retracts the shift's *marginal* contribution from its
// current employee (its per-shift penalty plus its pairs with that employee's
// other shifts); afterVariableChanged inserts it into the new employee —
// proportional to shifts per employee, independent of the total shift count.
// Unassigned shifts (employeeIndex == -1) contribute nothing, so the
// construction heuristic can score partially initialized solutions. All
// helpers are shared with ScoreCalculator's full recompute, so the incremental
// score is correct by construction.
class EmployeeSchedulingIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<EmployeeSchedule> {
public:
    static constexpr std::size_t DEFAULT_PAIR_CACHE_BUDGET_BYTES = 64U * 1024U * 1024U;

    explicit EmployeeSchedulingIncrementalScoreCalculator(
            std::size_t pairCacheBudgetBytes = DEFAULT_PAIR_CACHE_BUDGET_BYTES)
        : pairCacheBudgetBytes_(pairCacheBudgetBytes) {}

    void resetWorkingSolution(EmployeeSchedule& workingSolution) override;
    void beforeVariableChanged(int shiftIndex) override;
    void afterVariableChanged(int shiftIndex) override;
    Score calculateScore() override;

    // For violation-guided move selection: shift lists per employee, and the
    // set of employees that currently have a hard-constraint penalty (>0).
    const std::vector<std::vector<int>>& employeeShifts() const { return empShifts_; }
    const std::vector<int>& penalizedEmployees() const { return penalized_; }
    bool usesPairPenaltyCache() const { return pairPenaltyCached_; }

private:
    EmployeeSchedule* sched_ = nullptr;
    int n_ = 0;                                // employee count (balance items)
    std::vector<int> assign_;                  // employee per shift (-1 == unassigned)
    std::vector<std::vector<int>> empShifts_;  // shift indices per employee
    std::vector<int> shiftPosition_;            // position inside empShifts_[assign_[shift]]
    int shiftCount_ = 0;
    // Day-bucketed shift lists per employee for the sparse marginal path.
    // pairHardPenalty(a, b) is provably zero once the two start days differ by
    // more than dayWindow_ (overlap needs b.start < a.end <= a.start + maxDur;
    // min-rest needs b.start < a.end + MIN_REST; same-day is day distance 0),
    // so marginalOf scans only the 2*dayWindow_+1 buckets around the shift
    // instead of the employee's whole shift list. Exact by construction: the
    // window is conservative (derived from the max shift duration), and every
    // skipped pair contributes exactly 0. dayCount_ == 0 disables bucketing
    // (degenerate day ranges) and falls back to the flat empShifts_ scan.
    int minDay_ = 0;
    int dayCount_ = 0;
    int dayWindow_ = 0;
    std::vector<std::vector<int>> byDay_;  // (e * dayCount_) + (startDay - minDay_)
    std::vector<int> bucketPos_;           // shift -> position in its day bucket
    // Immutable score terms, flattened for cache-friendly hot-path lookup.
    // These replace tree lookups and branch-heavy interval arithmetic on
    // every candidate move.
    struct ShiftTerms {
        std::int64_t penalty;
        std::int64_t soft;
    };
    std::vector<ShiftTerms> shiftTerms_;       // employee * shiftCount + shift
    std::vector<std::int64_t> pairPenalty_;    // shift * shiftCount + shift
    std::size_t pairCacheBudgetBytes_;
    bool pairPenaltyCached_ = false;
    std::vector<std::int64_t> empPenalty_;     // hard penalty magnitude per employee (>= 0)
    std::vector<std::int64_t> empSoft_;        // net soft per employee
    std::int64_t totalPenalty_ = 0;            // sum of empPenalty_
    std::int64_t totalSoft_ = 0;               // sum of empSoft_
    long balSum_ = 0;                          // sum of counts (= assigned shifts)
    long balSumSq_ = 0;                        // sum of counts^2
    long cachedBalSum_ = 0;
    long cachedBalSumSq_ = 0;
    double cachedUnfairness_ = 0.0;
    bool unfairnessCached_ = false;
    // Lazy direct lookup for the fully assigned, <=1,023-shift hot path.
    std::vector<double> unfairnessBySumSq_;

    // Employees with a hard penalty > 0, as an unordered list with constant-time
    // membership/removal (penalizedPos_[e] = index in penalized_, or -1).
    std::vector<int> penalized_;
    std::vector<int> penalizedPos_;
    void setPenalized(int e, bool on);

    // Day-bucket maintenance for the sparse marginal path (no-ops disabled).
    void bucketInsert(int e, int s);
    void bucketRemove(int e, int s);

    // Full contribution of employee `e` given its shift list (used at reset).
    void contributionOf(int e, const std::vector<int>& list,
                        std::int64_t& penalty, std::int64_t& soft) const;
    // Marginal contribution of shift `s` to employee `e`: s's per-shift terms
    // plus its pairs with e's current shifts (skipping s itself).
    void marginalOf(int e, int s, std::int64_t& penalty, std::int64_t& soft) const;

    std::size_t employeeShiftOffset(int e, int s) const {
        return static_cast<std::size_t>(e) * shiftCount_ + s;
    }
    std::size_t pairOffset(int a, int b) const {
        return static_cast<std::size_t>(a) * shiftCount_ + b;
    }
    std::int64_t pairPenaltyOf(int a, int b) const;
};

}  // namespace es

#endif  // ES_INCREMENTAL_CALCULATOR_HPP

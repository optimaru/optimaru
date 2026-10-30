#include "es/incremental_calculator.hpp"

#include <limits>

#include "es/score_calculator.hpp"

namespace es {

void EmployeeSchedulingIncrementalScoreCalculator::resetWorkingSolution(
        EmployeeSchedule& workingSolution) {
    ScoreCalculator::prepare(workingSolution);
    sched_ = &workingSolution;
    n_ = static_cast<int>(workingSolution.employees.size());
    shiftCount_ = static_cast<int>(workingSolution.shifts.size());

    shiftTerms_.resize(static_cast<std::size_t>(n_) * shiftCount_);
    for (int e = 0; e < n_; ++e) {
        const Employee& employee = workingSolution.employees[e];
        for (int s = 0; s < shiftCount_; ++s) {
            const Shift& shift = workingSolution.shifts[s];
            const std::size_t offset = employeeShiftOffset(e, s);
            shiftTerms_[offset] = {ScoreCalculator::shiftHardPenalty(employee, shift),
                                   ScoreCalculator::shiftSoftDelta(employee, shift)};
        }
    }
    const std::size_t shifts = static_cast<std::size_t>(shiftCount_);
    const bool cellsFit = shifts == 0 || shifts <= std::numeric_limits<std::size_t>::max() / shifts;
    const std::size_t cells = cellsFit ? shifts * shifts : 0;
    pairPenaltyCached_ = cellsFit && cells <= pairCacheBudgetBytes_ / sizeof(std::int64_t);
    if (pairPenaltyCached_) {
        pairPenalty_.assign(cells, 0);
        for (int a = 0; a < shiftCount_; ++a) {
            for (int b = a + 1; b < shiftCount_; ++b) {
                const std::int64_t penalty = ScoreCalculator::pairHardPenalty(
                        workingSolution.shifts[a], workingSolution.shifts[b]);
                pairPenalty_[pairOffset(a, b)] = penalty;
                pairPenalty_[pairOffset(b, a)] = penalty;
            }
        }
    } else {
        pairPenalty_.clear();
        pairPenalty_.shrink_to_fit();
    }

    // Sparse-marginal day buckets: conservative interaction window from the
    // longest shift ((dayGap-1)*1440 < maxDur + MIN_REST is required for any
    // cross-day pair penalty; +1 day of slack keeps it provably inclusive).
    minDay_ = 0;
    dayCount_ = 0;
    dayWindow_ = 0;
    byDay_.clear();
    bucketPos_.assign(workingSolution.shifts.size(), -1);
    if (shiftCount_ > 0) {
        int minDay = workingSolution.shifts[0].startDay;
        int maxDay = minDay;
        Minutes maxDuration = 0;
        for (const Shift& shift : workingSolution.shifts) {
            minDay = std::min(minDay, shift.startDay);
            maxDay = std::max(maxDay, shift.startDay);
            maxDuration = std::max(maxDuration, shift.end - shift.start);
        }
        const long long spanDays = static_cast<long long>(maxDay) - minDay + 1;
        // +2 days of slack: one for the sub-day remainders of both starts,
        // one more because dayOf() truncates toward zero (a negative start
        // sits up to a full extra day below startDay * MINUTES_PER_DAY).
        const int window = 2 + static_cast<int>((maxDuration +
                                                 ScoreCalculator::MIN_REST_MINUTES) /
                                                MINUTES_PER_DAY);
        // Bucketing only pays when the flat per-employee list is clearly
        // longer than the windowed scan (per-bucket iteration and the constant-time
        // insert/remove maintenance are not free). Cost model: flat cost ~
        // shifts-per-employee; bucket cost ~ window entries (spe * buckets /
        // days) + one probe per bucket + maintenance slack. Measured: flat wins
        // at about a dozen shifts per employee, bucketing wins clearly at 25.
        const double spe = n_ > 0 ? static_cast<double>(shiftCount_) / n_ : 0.0;
        const int buckets = 2 * window + 1;
        const bool worthIt =
                spanDays <= 4096 && n_ > 0 &&
                spe > buckets * (1.0 + spe / static_cast<double>(spanDays)) + 4.0;
        if (worthIt) {
            minDay_ = minDay;
            dayCount_ = static_cast<int>(spanDays);
            dayWindow_ = window;
            byDay_.assign(static_cast<std::size_t>(n_) * dayCount_, {});
        }
    }

    assign_.assign(workingSolution.shifts.size(), -1);
    shiftPosition_.assign(workingSolution.shifts.size(), -1);
    empShifts_.assign(n_, {});
    empPenalty_.assign(n_, 0);
    empSoft_.assign(n_, 0);
    totalPenalty_ = 0;
    totalSoft_ = 0;
    balSum_ = 0;
    balSumSq_ = 0;
    unfairnessCached_ = false;
    // With every shift assigned, unfairness is a pure function of sum(count^2).
    // A direct lazy table removes sqrt + six-significant-digit rounding from
    // the per-move path on rosters up to 1,023 shifts, while capping memory at
    // 8 MiB. Larger rosters retain the one-entry cache below.
    constexpr std::size_t maxCachedShiftCount = 1U << 10;
    if (static_cast<std::size_t>(shiftCount_) < maxCachedShiftCount) {
        const std::size_t maxSumSq = static_cast<std::size_t>(shiftCount_) * shiftCount_;
        unfairnessBySumSq_.assign(maxSumSq + 1, -1.0);
    } else {
        unfairnessBySumSq_.clear();
    }
    penalized_.clear();
    penalizedPos_.assign(n_, -1);

    for (int i = 0; i < static_cast<int>(workingSolution.shifts.size()); ++i) {
        int e = workingSolution.shifts[i].employeeIndex;
        assign_[i] = e;
        if (e >= 0) {
            shiftPosition_[i] = static_cast<int>(empShifts_[e].size());
            empShifts_[e].push_back(i);
            if (dayCount_ > 0) bucketInsert(e, i);
        }
    }
    for (int e = 0; e < n_; ++e) {
        contributionOf(e, empShifts_[e], empPenalty_[e], empSoft_[e]);
        totalPenalty_ += empPenalty_[e];
        totalSoft_ += empSoft_[e];
        long c = static_cast<long>(empShifts_[e].size());
        balSum_ += c;
        balSumSq_ += c * c;
        setPenalized(e, empPenalty_[e] > 0);
    }
}

void EmployeeSchedulingIncrementalScoreCalculator::setPenalized(int e, bool on) {
    int pos = penalizedPos_[e];
    if (on && pos == -1) {
        penalizedPos_[e] = static_cast<int>(penalized_.size());
        penalized_.push_back(e);
    } else if (!on && pos != -1) {
        int last = penalized_.back();
        penalized_[pos] = last;
        penalizedPos_[last] = pos;
        penalized_.pop_back();
        penalizedPos_[e] = -1;
    }
}

void EmployeeSchedulingIncrementalScoreCalculator::contributionOf(
        int e, const std::vector<int>& list, std::int64_t& penalty, std::int64_t& soft) const {
    penalty = 0;
    soft = 0;
    for (std::size_t a = 0; a < list.size(); ++a) {
        const int shiftA = list[a];
        const std::size_t offset = employeeShiftOffset(e, shiftA);
        penalty += shiftTerms_[offset].penalty;
        soft += shiftTerms_[offset].soft;
        for (std::size_t b = a + 1; b < list.size(); ++b) {
            penalty += pairPenaltyOf(shiftA, list[b]);
        }
    }
}

void EmployeeSchedulingIncrementalScoreCalculator::bucketInsert(int e, int s) {
    std::vector<int>& bucket =
            byDay_[static_cast<std::size_t>(e) * dayCount_ +
                   (sched_->shifts[s].startDay - minDay_)];
    bucketPos_[s] = static_cast<int>(bucket.size());
    bucket.push_back(s);
}

void EmployeeSchedulingIncrementalScoreCalculator::bucketRemove(int e, int s) {
    std::vector<int>& bucket =
            byDay_[static_cast<std::size_t>(e) * dayCount_ +
                   (sched_->shifts[s].startDay - minDay_)];
    const int pos = bucketPos_[s];
    const int last = bucket.back();
    bucket[static_cast<std::size_t>(pos)] = last;
    bucketPos_[last] = pos;
    bucket.pop_back();
    bucketPos_[s] = -1;
}

void EmployeeSchedulingIncrementalScoreCalculator::marginalOf(
        int e, int s, std::int64_t& penalty, std::int64_t& soft) const {
    const std::size_t offset = employeeShiftOffset(e, s);
    penalty = shiftTerms_[offset].penalty;
    soft = shiftTerms_[offset].soft;
    if (dayCount_ == 0) {
        // Flat path (small rosters): one contiguous scan of the employee's
        // shift list. Kept first so the common small-instance case stays the
        // fallthrough branch.
        for (int t : empShifts_[e]) {
            if (t == s) continue;
            penalty += pairPenaltyOf(s, t);
        }
        return;
    }
    // Sparse path: only the day buckets inside the interaction window can
    // hold a nonzero pair partner (see the dayWindow_ derivation above).
    const int day = static_cast<int>(sched_->shifts[s].startDay) - minDay_;
    const int lo = day - dayWindow_ > 0 ? day - dayWindow_ : 0;
    const int hi = day + dayWindow_ < dayCount_ - 1 ? day + dayWindow_ : dayCount_ - 1;
    const std::size_t base = static_cast<std::size_t>(e) * dayCount_;
    for (int d = lo; d <= hi; ++d) {
        for (int t : byDay_[base + d]) {
            if (t == s) continue;
            penalty += pairPenaltyOf(s, t);
        }
    }
}

std::int64_t EmployeeSchedulingIncrementalScoreCalculator::pairPenaltyOf(int a, int b) const {
    if (pairPenaltyCached_) return pairPenalty_[pairOffset(a, b)];
    return ScoreCalculator::pairHardPenalty(sched_->shifts[a], sched_->shifts[b]);
}

void EmployeeSchedulingIncrementalScoreCalculator::beforeVariableChanged(int shiftIndex) {
    int e = assign_[shiftIndex];
    if (e < 0) return;  // unassigned: nothing to retract

    const bool wasPenalized = empPenalty_[e] > 0;
    std::int64_t remPen, remSoft;
    marginalOf(e, shiftIndex, remPen, remSoft);
    empPenalty_[e] -= remPen;
    empSoft_[e] -= remSoft;
    totalPenalty_ -= remPen;
    totalSoft_ -= remSoft;

    long c = static_cast<long>(empShifts_[e].size());
    balSumSq_ += (c - 1) * (c - 1) - c * c;
    balSum_ -= 1;
    auto& list = empShifts_[e];
    const int pos = shiftPosition_[shiftIndex];
    const int last = list.back();
    list[static_cast<std::size_t>(pos)] = last;
    shiftPosition_[last] = pos;
    list.pop_back();
    shiftPosition_[shiftIndex] = -1;
    if (dayCount_ > 0) bucketRemove(e, shiftIndex);

    // The hot list changes only when this employee crosses hard zero.
    const bool isPenalized = empPenalty_[e] > 0;
    if (wasPenalized != isPenalized) setPenalized(e, isPenalized);
    assign_[shiftIndex] = -1;
}

void EmployeeSchedulingIncrementalScoreCalculator::afterVariableChanged(int shiftIndex) {
    int e = sched_->shifts[shiftIndex].employeeIndex;  // the new value
    assign_[shiftIndex] = e;
    if (e < 0) return;  // now unassigned: nothing to insert

    const bool wasPenalized = empPenalty_[e] > 0;
    std::int64_t addPen, addSoft;
    marginalOf(e, shiftIndex, addPen, addSoft);  // shiftIndex not yet in empShifts_[e]
    empPenalty_[e] += addPen;
    empSoft_[e] += addSoft;
    totalPenalty_ += addPen;
    totalSoft_ += addSoft;

    long c = static_cast<long>(empShifts_[e].size());
    balSumSq_ += (c + 1) * (c + 1) - c * c;
    balSum_ += 1;
    shiftPosition_[shiftIndex] = static_cast<int>(empShifts_[e].size());
    empShifts_[e].push_back(shiftIndex);
    if (dayCount_ > 0) bucketInsert(e, shiftIndex);

    const bool isPenalized = empPenalty_[e] > 0;
    if (wasPenalized != isPenalized) setPenalized(e, isPenalized);
}

Score EmployeeSchedulingIncrementalScoreCalculator::calculateScore() {
    double unfairness;
    if (balSum_ == shiftCount_ && balSumSq_ >= 0
        && static_cast<std::size_t>(balSumSq_) < unfairnessBySumSq_.size()) {
        double& cached = unfairnessBySumSq_[static_cast<std::size_t>(balSumSq_)];
        if (cached < 0.0)
            cached = ScoreCalculator::unfairnessFromAggregates(balSum_, balSumSq_, n_);
        unfairness = cached;
    } else {
        if (!unfairnessCached_ || cachedBalSum_ != balSum_
            || cachedBalSumSq_ != balSumSq_) {
            cachedUnfairness_ =
                    ScoreCalculator::unfairnessFromAggregates(balSum_, balSumSq_, n_);
            cachedBalSum_ = balSum_;
            cachedBalSumSq_ = balSumSq_;
            unfairnessCached_ = true;
        }
        unfairness = cachedUnfairness_;
    }
    Score sc;
    sc.hard = -totalPenalty_;
    sc.soft = static_cast<double>(totalSoft_) - unfairness;
    return sc;
}

}  // namespace es

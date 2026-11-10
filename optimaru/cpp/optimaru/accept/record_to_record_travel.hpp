// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_ACCEPT_RECORD_TO_RECORD_TRAVEL_HPP
#define OPTIMARU_ACCEPT_RECORD_TO_RECORD_TRAVEL_HPP

#include <algorithm>
#include <cmath>
#include <random>

#include "optimaru/accept/acceptor.hpp"
#include "optimaru/score/score.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Record-to-record travel (Dueck 1993)
// ---------------------------------------------------------------------------
// Keeps a candidate that trails the best score so far by no more than a
// threshold that shrinks linearly every step:
//
//     keep  <=>  best.soft - candidate.soft <= threshold
//     threshold <- max(end_threshold, threshold - step)
//
// Santini, Ropke & Hvattum (2018) found RRT competitive with — often better
// than — simulated annealing across ALNS benchmarks, and it has one practical
// advantage here: its threshold is in score units, so it can be fitted to the
// initial objective without a temperature to guess (see autofit()).
//
// Feasibility is never traded away: a candidate whose hard score is worse than
// the best's is refused outright, and one that is better is always kept.
// The threshold governs the soft level only, which is where the engine's
// medium level is packed on multi-level domains.
class RecordToRecordTravel final : public Acceptor {
public:
    RecordToRecordTravel(double start_threshold, double end_threshold, double step)
        : end_threshold_(end_threshold), step_(step), threshold_(start_threshold) {}

    // A threshold schedule from fractions of the initial objective: start at
    // start_gap of |initial_soft|, end at end_gap, reaching the end in
    // `steps` steps.
    static RecordToRecordTravel autofit(double initial_soft, double start_gap, double end_gap,
                                        long long steps) {
        const double magnitude = std::fabs(initial_soft);
        const double start = start_gap * magnitude;
        const double end = end_gap * magnitude;
        const auto n = static_cast<double>(steps < 1 ? 1 : steps);
        return RecordToRecordTravel(start, end, (start - end) / n);
    }

    bool accepts(const Score& candidate, const Score&, const Score& best, const int*, int,
                 double, std::mt19937_64&) override {
        const bool accepted = candidate.hard != best.hard
                                      ? candidate.hard > best.hard
                                      : best.soft - candidate.soft <= threshold_;
        threshold_ = std::max(end_threshold_, threshold_ - step_);
        return accepted;
    }

private:
    double end_threshold_;
    double step_;
    double threshold_;
};

}  // namespace optimaru

#endif  // OPTIMARU_ACCEPT_RECORD_TO_RECORD_TRAVEL_HPP

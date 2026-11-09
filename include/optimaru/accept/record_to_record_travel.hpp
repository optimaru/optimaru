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
// Record-to-record travel (Dueck 1993) / threshold accepting (Dueck 1990)
// ---------------------------------------------------------------------------
// Keeps a candidate that trails the baseline by no more than a threshold that
// shrinks every step:
//
//     keep  <=>  baseline.soft - candidate.soft <= threshold
//     threshold <- max(end_threshold, threshold - step)          (linear)
//     threshold <- max(end_threshold, threshold * step)          (exponential)
//
// `compare_to_best` picks the baseline: the best score so far gives classic
// RRT (the search may wander a fixed distance below the record), the current
// score gives threshold accepting (the wander is relative to where it already
// is).
//
// Santini, Ropke & Hvattum (2018) found RRT competitive with — often better
// than — simulated annealing across ALNS benchmarks, and it has one practical
// advantage here: its threshold is in score units, so it can be fitted to the
// initial objective without a temperature to guess (see autofit()).
//
// Feasibility is never traded away: a candidate whose hard score is worse than
// the baseline's is refused outright, and one that is better is always kept.
// The threshold governs the soft level only, which is where the engine's
// medium level is packed on multi-level domains.
class RecordToRecordTravel final : public Acceptor {
public:
    enum class Method { Linear, Exponential };

    RecordToRecordTravel(double start_threshold, double end_threshold, double step,
                         Method method = Method::Linear, bool compare_to_best = true)
        : end_threshold_(end_threshold),
          step_(step),
          method_(method),
          compare_to_best_(compare_to_best),
          threshold_(start_threshold) {}

    // A threshold schedule from fractions of the initial objective: start at
    // start_gap of |initial_soft|, end at end_gap, reaching the end in
    // `steps` steps.
    static RecordToRecordTravel autofit(double initial_soft, double start_gap, double end_gap,
                                        long long steps, Method method = Method::Linear,
                                        bool compare_to_best = true) {
        const double magnitude = std::fabs(initial_soft);
        const double start = start_gap * magnitude;
        const double end = end_gap * magnitude;
        const auto n = static_cast<double>(steps < 1 ? 1 : steps);
        double step;
        if (method == Method::Linear) {
            step = (start - end) / n;
        } else {
            // Guard the degenerate start == 0 case: any ratio would be
            // undefined, and a zero-threshold RRT is just hill climbing.
            step = start > 0.0 ? std::pow(end / start, 1.0 / n) : 1.0;
        }
        return RecordToRecordTravel(start, end, step, method, compare_to_best);
    }

    bool accepts(const Score& candidate, const Score& current, const Score& best, const int*,
                 int, double, std::mt19937_64&) override {
        const Score& baseline = compare_to_best_ ? best : current;
        const bool accepted = candidate.hard != baseline.hard
                                      ? candidate.hard > baseline.hard
                                      : baseline.soft - candidate.soft <= threshold_;
        advance();
        return accepted;
    }

private:
    void advance() {
        const double next = method_ == Method::Linear ? threshold_ - step_ : threshold_ * step_;
        threshold_ = std::max(end_threshold_, next);
    }

    double end_threshold_;
    double step_;
    Method method_;
    bool compare_to_best_;
    double threshold_;
};

}  // namespace optimaru

#endif  // OPTIMARU_ACCEPT_RECORD_TO_RECORD_TRAVEL_HPP

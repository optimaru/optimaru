#ifndef OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_RECORDTORECORDTRAVEL_RECORD_TO_RECORD_TRAVEL_ACCEPTOR_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_RECORDTORECORDTRAVEL_RECORD_TO_RECORD_TRAVEL_ACCEPTOR_HPP

#include <algorithm>
#include <cmath>

#include "optimaru/impl/localsearch/decider/acceptor/acceptor.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Record-to-Record Travel (Dueck 1993) / Threshold Accepting (Dueck 1990)
// ---------------------------------------------------------------------------
// Record-to-record travel accepts a candidate
// when it trails the baseline by no more than a threshold that shrinks every
// step:
//
//     accept  <=>  baseline.soft - candidate.soft <= threshold
//     threshold <- max(endThreshold, threshold - step)          (linear)
//     threshold <- max(endThreshold, threshold * step)          (exponential)
//
// `compareToBest` picks the baseline: the best score so far gives classic RRT
// (the search may wander a fixed distance below the record), the current score
// gives threshold accepting (the wander is relative to where it already is).
//
// Santini, Ropke & Hvattum (2018) found RRT competitive with — often better
// than — simulated annealing across ALNS benchmarks, and it has one practical
// advantage here: its threshold is in score units, so it can be autofitted
// from the initial objective without a temperature to guess (see autofit()).
//
// Feasibility is never traded away: a candidate whose hard score is worse than
// the baseline's is rejected outright, and one that is better is always
// accepted. The threshold governs the soft level only, which is where the
// engine's medium level is packed on multi-level domains.
class RecordToRecordTravelAcceptor final : public Acceptor {
public:
    enum class Method { LINEAR, EXPONENTIAL };

    RecordToRecordTravelAcceptor(double startThreshold, double endThreshold, double step,
                                 Method method = Method::LINEAR, bool compareToBest = true)
        : endThreshold_(endThreshold),
          step_(step),
          method_(method),
          compareToBest_(compareToBest),
          threshold_(startThreshold) {}

    // Threshold schedule from a fraction of the initial objective, mirroring
    // RecordToRecordTravel.autofit: start at startGap of |initialSoft|, end at
    // endGap, reaching the end in `steps` steps.
    static RecordToRecordTravelAcceptor autofit(double initialSoft, double startGap,
                                                double endGap, long long steps,
                                                Method method = Method::LINEAR,
                                                bool compareToBest = true) {
        const double magnitude = std::fabs(initialSoft);
        const double start = startGap * magnitude;
        const double end = endGap * magnitude;
        const auto n = static_cast<double>(steps < 1 ? 1 : steps);
        double step;
        if (method == Method::LINEAR) {
            step = (start - end) / n;
        } else {
            // Guard the degenerate start == 0 case: any ratio would be
            // undefined, and a zero-threshold RRT is just hill climbing.
            step = start > 0.0 ? std::pow(end / start, 1.0 / n) : 1.0;
        }
        return RecordToRecordTravelAcceptor(start, end, step, method, compareToBest);
    }

    bool isAccepted(const HardSoftScore& candidate, const HardSoftScore& current,
                    const HardSoftScore& best, const int*, int, double,
                    std::mt19937_64&) override {
        const HardSoftScore& baseline = compareToBest_ ? best : current;
        const bool accepted = candidate.hard != baseline.hard
                                      ? candidate.hard > baseline.hard
                                      : baseline.soft - candidate.soft <= threshold_;
        advance();
        return accepted;
    }

    double threshold() const { return threshold_; }

private:
    void advance() {
        const double next =
                method_ == Method::LINEAR ? threshold_ - step_ : threshold_ * step_;
        threshold_ = std::max(endThreshold_, next);
    }

    double endThreshold_;
    double step_;
    Method method_;
    bool compareToBest_;
    double threshold_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_RECORDTORECORDTRAVEL_RECORD_TO_RECORD_TRAVEL_ACCEPTOR_HPP

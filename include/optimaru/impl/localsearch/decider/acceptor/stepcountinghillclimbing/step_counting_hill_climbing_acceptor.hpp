#ifndef OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_STEPCOUNTINGHILLCLIMBING_STEP_COUNTING_HILL_CLIMBING_ACCEPTOR_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_STEPCOUNTINGHILLCLIMBING_STEP_COUNTING_HILL_CLIMBING_ACCEPTOR_HPP

#include <cstdint>

#include "optimaru/impl/localsearch/decider/acceptor/acceptor.hpp"

namespace optimaru {

// Step Counting Hill Climbing (port of impl.localsearch.decider.acceptor.
// stepcountinghillclimbing.StepCountingHillClimbingAcceptor): like Late
// Acceptance, but the threshold is a single score refreshed to the current
// score every `size` steps, instead of a rolling history.
class StepCountingHillClimbingAcceptor final : public Acceptor {
public:
    StepCountingHillClimbingAcceptor(std::int64_t size, const HardSoftScore& initialScore)
        : size_(size), threshold_(initialScore) {}

    bool isAccepted(const HardSoftScore& candidate, const HardSoftScore& current,
                    const HardSoftScore&, const int*, int, double, std::mt19937_64&) override {
        return candidate >= current || candidate >= threshold_;
    }
    void stepEnded(const HardSoftScore& current) override {
        if (++count_ >= size_) {
            threshold_ = current;
            count_ = 0;
        }
    }

private:
    std::int64_t size_;
    std::int64_t count_ = 0;
    HardSoftScore threshold_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_STEPCOUNTINGHILLCLIMBING_STEP_COUNTING_HILL_CLIMBING_ACCEPTOR_HPP

#ifndef OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_HILLCLIMBING_HILL_CLIMBING_ACCEPTOR_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_HILLCLIMBING_HILL_CLIMBING_ACCEPTOR_HPP

#include "optimaru/impl/localsearch/decider/acceptor/acceptor.hpp"

namespace optimaru {

// Accepts any move that is not worse than the current score.
class HillClimbingAcceptor final : public Acceptor {
public:
    bool isAccepted(const HardSoftScore& candidate, const HardSoftScore& current,
                    const HardSoftScore&, const int*, int, double, std::mt19937_64&) override {
        return candidate >= current;
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_HILLCLIMBING_HILL_CLIMBING_ACCEPTOR_HPP

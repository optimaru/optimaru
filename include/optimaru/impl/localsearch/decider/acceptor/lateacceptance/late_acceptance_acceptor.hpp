#ifndef OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_LATEACCEPTANCE_LATE_ACCEPTANCE_ACCEPTOR_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_LATEACCEPTANCE_LATE_ACCEPTANCE_ACCEPTOR_HPP

#include <cstddef>
#include <vector>

#include "optimaru/impl/localsearch/decider/acceptor/acceptor.hpp"

namespace optimaru {

// Late Acceptance Hill Climbing: accept if not worse than the current score,
// or than the score from lateAcceptanceSize steps ago.
class LateAcceptanceAcceptor final : public Acceptor {
public:
    LateAcceptanceAcceptor(std::size_t lateAcceptanceSize, const HardSoftScore& initialScore)
        : history_(lateAcceptanceSize, initialScore) {}

    bool isAccepted(const HardSoftScore& candidate, const HardSoftScore& current,
                    const HardSoftScore&, const int*, int, double, std::mt19937_64&) override {
        return candidate >= current || candidate >= history_[pos_];
    }
    void stepEnded(const HardSoftScore& current) override {
        history_[pos_] = current;
        pos_ = (pos_ + 1) % history_.size();
    }

private:
    std::vector<HardSoftScore> history_;
    std::size_t pos_ = 0;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_LATEACCEPTANCE_LATE_ACCEPTANCE_ACCEPTOR_HPP

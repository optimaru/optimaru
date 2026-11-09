// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_ACCEPT_STEP_COUNTING_HILL_CLIMBING_HPP
#define OPTIMARU_ACCEPT_STEP_COUNTING_HILL_CLIMBING_HPP

#include <cstdint>
#include <random>

#include "optimaru/accept/acceptor.hpp"
#include "optimaru/score/score.hpp"

namespace optimaru {

// Step counting hill climbing (Bykov & Petrovic): like late acceptance, but
// the bar is a single score, reset to the current score every `interval`
// steps, instead of a rolling history.
class StepCountingHillClimbing final : public Acceptor {
public:
    StepCountingHillClimbing(std::int64_t interval, const Score& initial_score)
        : interval_(interval), bar_(initial_score) {}

    bool accepts(const Score& candidate, const Score& current, const Score&, const int*, int,
                 double, std::mt19937_64&) override {
        return candidate >= current || candidate >= bar_;
    }
    void on_step_end(const Score& current) override {
        if (++count_ >= interval_) {
            bar_ = current;
            count_ = 0;
        }
    }

private:
    std::int64_t interval_;
    std::int64_t count_ = 0;
    Score bar_;
};

}  // namespace optimaru

#endif  // OPTIMARU_ACCEPT_STEP_COUNTING_HILL_CLIMBING_HPP

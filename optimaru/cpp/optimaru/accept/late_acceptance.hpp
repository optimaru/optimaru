// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_ACCEPT_LATE_ACCEPTANCE_HPP
#define OPTIMARU_ACCEPT_LATE_ACCEPTANCE_HPP

#include <cstddef>
#include <random>
#include <vector>

#include "optimaru/accept/acceptor.hpp"
#include "optimaru/score/score.hpp"

namespace optimaru {

// Late acceptance hill climbing (Burke & Bykov): keep a candidate that is not
// worse than the current score, or than the score `history_length` steps ago.
class LateAcceptance final : public Acceptor {
public:
    LateAcceptance(std::size_t history_length, const Score& initial_score)
        : history_(history_length, initial_score) {}

    bool accepts(const Score& candidate, const Score& current, const Score&, const int*, int,
                 double, std::mt19937_64&) override {
        return candidate >= current || candidate >= history_[pos_];
    }
    void on_step_end(const Score& current) override {
        history_[pos_] = current;
        pos_ = (pos_ + 1) % history_.size();
    }

private:
    std::vector<Score> history_;
    std::size_t pos_ = 0;
};

}  // namespace optimaru

#endif  // OPTIMARU_ACCEPT_LATE_ACCEPTANCE_HPP

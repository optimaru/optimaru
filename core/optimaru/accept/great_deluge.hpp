// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_ACCEPT_GREAT_DELUGE_HPP
#define OPTIMARU_ACCEPT_GREAT_DELUGE_HPP

#include <random>

#include "optimaru/accept/acceptor.hpp"
#include "optimaru/score/score.hpp"

namespace optimaru {

// Great deluge (Dueck 1993): keep any candidate at or above a "water level"
// that starts at the initial score and rises by a fixed step every step, so
// the acceptable region tightens over time. A zero step degenerates to hill
// climbing anchored at the initial score.
class GreatDeluge final : public Acceptor {
public:
    GreatDeluge(const Score& initial_score, const Score& level_step)
        : level_(initial_score), step_(level_step) {}

    bool accepts(const Score& candidate, const Score& current, const Score&, const int*, int,
                 double, std::mt19937_64&) override {
        return candidate >= current || candidate >= level_;
    }
    void on_step_end(const Score&) override {
        level_.hard += step_.hard;
        level_.soft += step_.soft;
    }

private:
    Score level_;
    Score step_;
};

}  // namespace optimaru

#endif  // OPTIMARU_ACCEPT_GREAT_DELUGE_HPP

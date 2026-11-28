// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_ACCEPT_HILL_CLIMBING_HPP
#define OPTIMARU_ACCEPT_HILL_CLIMBING_HPP

#include <random>

#include "optimaru/accept/acceptor.hpp"
#include "optimaru/score/score.hpp"

namespace optimaru {

// Keep a candidate that is not worse than the current score.
class HillClimbing final : public Acceptor {
public:
    bool accepts(const Score& candidate, const Score& current, const Score&, const int*, int,
                 double, std::mt19937_64&) override {
        return candidate >= current;
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_ACCEPT_HILL_CLIMBING_HPP

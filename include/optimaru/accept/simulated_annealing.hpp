// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_ACCEPT_SIMULATED_ANNEALING_HPP
#define OPTIMARU_ACCEPT_SIMULATED_ANNEALING_HPP

#include <cmath>
#include <random>

#include "optimaru/accept/acceptor.hpp"
#include "optimaru/score/score.hpp"

namespace optimaru {

// Simulated annealing (Kirkpatrick et al.): a worse candidate is kept with
// probability exp(delta / temperature), taken per score level, with the
// temperature falling linearly from `start_temperature` to 0 as `progress`
// runs from 0 to 1.
class SimulatedAnnealing final : public Acceptor {
public:
    explicit SimulatedAnnealing(const Score& start_temperature)
        : start_temperature_(start_temperature) {}

    bool accepts(const Score& candidate, const Score& current, const Score&, const int*, int,
                 double progress, std::mt19937_64& rng) override {
        if (candidate >= current) return true;
        double probability = 1.0;
        double hard_delta = static_cast<double>(candidate.hard - current.hard);
        if (hard_delta < 0) {
            double temp = static_cast<double>(start_temperature_.hard) * (1.0 - progress);
            if (temp <= 0) return false;
            probability *= std::exp(hard_delta / temp);
        }
        double soft_delta = candidate.soft - current.soft;
        if (soft_delta < 0) {
            double temp = start_temperature_.soft * (1.0 - progress);
            if (temp <= 0) return false;
            probability *= std::exp(soft_delta / temp);
        }
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        return unit(rng) < probability;
    }

private:
    Score start_temperature_;
};

}  // namespace optimaru

#endif  // OPTIMARU_ACCEPT_SIMULATED_ANNEALING_HPP

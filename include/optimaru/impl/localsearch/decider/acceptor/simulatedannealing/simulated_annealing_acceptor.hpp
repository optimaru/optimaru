#ifndef OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_SIMULATEDANNEALING_SIMULATED_ANNEALING_ACCEPTOR_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_SIMULATEDANNEALING_SIMULATED_ANNEALING_ACCEPTOR_HPP

#include <cmath>

#include "optimaru/impl/localsearch/decider/acceptor/acceptor.hpp"

namespace optimaru {

// Simulated Annealing: a worse move is accepted with probability
// exp(delta / temperature), per score level, with the temperature cooling
// linearly from startingTemperature to ~0 over the solve's time limit
// (time-gradient cooling).
class SimulatedAnnealingAcceptor final : public Acceptor {
public:
    explicit SimulatedAnnealingAcceptor(const HardSoftScore& startingTemperature)
        : startingTemperature_(startingTemperature) {}

    bool isAccepted(const HardSoftScore& candidate, const HardSoftScore& current,
                    const HardSoftScore&, const int*, int, double timeGradient,
                    std::mt19937_64& rng) override {
        if (candidate >= current) return true;
        double probability = 1.0;
        double hardDelta = static_cast<double>(candidate.hard - current.hard);
        if (hardDelta < 0) {
            double temp = static_cast<double>(startingTemperature_.hard) * (1.0 - timeGradient);
            if (temp <= 0) return false;
            probability *= std::exp(hardDelta / temp);
        }
        double softDelta = candidate.soft - current.soft;
        if (softDelta < 0) {
            double temp = startingTemperature_.soft * (1.0 - timeGradient);
            if (temp <= 0) return false;
            probability *= std::exp(softDelta / temp);
        }
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        return unit(rng) < probability;
    }

private:
    HardSoftScore startingTemperature_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_SIMULATEDANNEALING_SIMULATED_ANNEALING_ACCEPTOR_HPP

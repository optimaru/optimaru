#ifndef OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_GREATDELUGE_GREAT_DELUGE_ACCEPTOR_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_GREATDELUGE_GREAT_DELUGE_ACCEPTOR_HPP

#include "optimaru/impl/localsearch/decider/acceptor/acceptor.hpp"

namespace optimaru {

// Great Deluge (port of impl.localsearch.decider.acceptor.greatdeluge.
// GreatDelugeAcceptor): accept any candidate at or above the "water level",
// which starts at the initial score and rises by a fixed increment every
// step, tightening the acceptable region over time. A zero increment
// degenerates to hill climbing anchored at the initial score.
class GreatDelugeAcceptor final : public Acceptor {
public:
    GreatDelugeAcceptor(const HardSoftScore& initialScore,
                        const HardSoftScore& waterLevelIncrement)
        : waterLevel_(initialScore), increment_(waterLevelIncrement) {}

    bool isAccepted(const HardSoftScore& candidate, const HardSoftScore& current,
                    const HardSoftScore&, const int*, int, double, std::mt19937_64&) override {
        return candidate >= current || candidate >= waterLevel_;
    }
    void stepEnded(const HardSoftScore&) override {
        waterLevel_.hard += increment_.hard;
        waterLevel_.soft += increment_.soft;
    }

private:
    HardSoftScore waterLevel_;
    HardSoftScore increment_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_GREATDELUGE_GREAT_DELUGE_ACCEPTOR_HPP

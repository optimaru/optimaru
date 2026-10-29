#ifndef OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_TABU_ENTITY_TABU_ACCEPTOR_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_TABU_ENTITY_TABU_ACCEPTOR_HPP

#include <cstdint>
#include <unordered_map>

#include "optimaru/impl/localsearch/decider/acceptor/acceptor.hpp"

namespace optimaru {

// Entity tabu: an accepted move marks its entities tabu for tabuSize steps;
// a move touching a tabu entity is rejected unless it beats the best score
// (aspiration). Among non-tabu moves this behaves like hill climbing — the
// tabu memory is what forces the search away from recently visited ground.
// (Classic tabu search evaluates a whole neighborhood per step; this is the
// single-candidate-per-step adaptation.)
class EntityTabuAcceptor final : public Acceptor {
public:
    explicit EntityTabuAcceptor(int tabuSize) : tabuSize_(tabuSize) {}

    bool isAccepted(const HardSoftScore& candidate, const HardSoftScore& current,
                    const HardSoftScore& best, const int* moveEntities, int moveEntityCount,
                    double, std::mt19937_64&) override {
        for (int i = 0; i < moveEntityCount; ++i) {
            auto it = tabuUntil_.find(moveEntities[i]);
            if (it != tabuUntil_.end() && it->second > step_ && !(candidate > best)) {
                return false;  // tabu, and no aspiration
            }
        }
        return candidate >= current;
    }
    void moveAccepted(const int* moveEntities, int moveEntityCount) override {
        for (int i = 0; i < moveEntityCount; ++i) {
            tabuUntil_[moveEntities[i]] = step_ + tabuSize_;
        }
    }
    void stepEnded(const HardSoftScore&) override { ++step_; }

private:
    int tabuSize_;
    std::int64_t step_ = 0;
    std::unordered_map<int, std::int64_t> tabuUntil_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_TABU_ENTITY_TABU_ACCEPTOR_HPP

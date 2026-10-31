#ifndef OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_ACCEPTOR_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_ACCEPTOR_HPP

#include <random>

#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Acceptor
// ---------------------------------------------------------------------------
// Mirrors impl.localsearch.decider.acceptor.Acceptor: the local-search
// acceptance strategy. The engine samples one candidate move per step,
// applies it, and asks the acceptor; a rejected move is undone.
//
//   isAccepted(...)      — decide on the (already applied) candidate.
//   moveAccepted(...)    — notify that the candidate was kept (tabu memory).
//   stepEnded(current)   — end of step, whatever the outcome (LA history).
//
// moveEntities identifies what the move touched (entity indices for BASIC,
// element ids for LIST) — used by entity tabu. timeGradient runs 0 -> 1 over
// the solve's spentLimit (used by simulated annealing's cooling schedule).
//
// Concrete acceptors live in their upstream-mirrored subdirectories:
// hillclimbing/, lateacceptance/, tabu/, simulatedannealing/, greatdeluge/,
// stepcountinghillclimbing/, recordtorecordtravel/.
class Acceptor {
public:
    virtual ~Acceptor() = default;

    virtual bool isAccepted(const HardSoftScore& candidate, const HardSoftScore& current,
                            const HardSoftScore& best, const int* moveEntities,
                            int moveEntityCount, double timeGradient, std::mt19937_64& rng) = 0;
    virtual void moveAccepted(const int* /*moveEntities*/, int /*moveEntityCount*/) {}
    virtual void stepEnded(const HardSoftScore& /*current*/) {}
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_DECIDER_ACCEPTOR_ACCEPTOR_HPP

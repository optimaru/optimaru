// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_ACCEPT_ACCEPTOR_HPP
#define OPTIMARU_ACCEPT_ACCEPTOR_HPP

#include <random>

#include "optimaru/score/score.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Acceptor
// ---------------------------------------------------------------------------
// Decides whether local search keeps a candidate move. Each step the search
// samples one move, scores it, and asks:
//
//   accepts(candidate, current, best, ...)   keep this candidate?
//   on_accepted(entities, count)              it was kept (tabu memory)
//   on_step_end(current)                      the step is over, whatever the
//                                             outcome (history-based rules)
//
// `move_entities` lists what the move touched (entity indices on the
// assignment shape, element ids on the sequence shape); only entity tabu reads
// it. `progress` runs 0 -> 1 over the solve's limits (see
// StopCondition::progress); only simulated annealing reads it.
class Acceptor {
public:
    virtual ~Acceptor() = default;

    virtual bool accepts(const Score& candidate, const Score& current, const Score& best,
                         const int* move_entities, int move_entity_count, double progress,
                         std::mt19937_64& rng) = 0;
    virtual void on_accepted(const int* /*move_entities*/, int /*move_entity_count*/) {}
    virtual void on_step_end(const Score& /*current*/) {}
};

}  // namespace optimaru

#endif  // OPTIMARU_ACCEPT_ACCEPTOR_HPP

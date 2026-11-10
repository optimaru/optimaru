// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SCORE_CHAIN_OBJECTIVE_HPP
#define OPTIMARU_SCORE_CHAIN_OBJECTIVE_HPP

#include <vector>

#include "optimaru/score/score.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// ChainObjective
// ---------------------------------------------------------------------------
// The objective interface for chain-shaped solutions. A chain is stored as
// backward `previous` links, which cannot be walked forward cheaply, so the
// engine hands the affected chain's CONTENT (entity ids in order from the
// anchor) straight to the hooks:
//
//   before_chain_edit(solution, anchor, chain)  // chain = OLD contents: retract
//   <the engine relinks the previous pointers>
//   after_chain_edit(solution, anchor, chain)   // chain = NEW contents: add back
//
// rebuild receives all chains. The usual incremental contract applies: after
// any hook sequence, score() equals a full recompute (checked under
// ScoreChecks::Full).
template <typename Solution>
class ChainObjective {
public:
    virtual ~ChainObjective() = default;

    virtual void rebuild(Solution& solution, const std::vector<std::vector<int>>& chains) = 0;
    virtual void before_chain_edit(Solution& /*solution*/, int /*anchor_index*/,
                                   const std::vector<int>& /*chain*/) {}
    virtual void after_chain_edit(Solution& /*solution*/, int /*anchor_index*/,
                                  const std::vector<int>& /*chain*/) {}
    virtual Score score() = 0;
};

}  // namespace optimaru

#endif  // OPTIMARU_SCORE_CHAIN_OBJECTIVE_HPP

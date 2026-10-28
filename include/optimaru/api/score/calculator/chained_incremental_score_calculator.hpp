#ifndef OPTIMARU_API_SCORE_CALCULATOR_CHAINED_INCREMENTAL_SCORE_CALCULATOR_HPP
#define OPTIMARU_API_SCORE_CALCULATOR_CHAINED_INCREMENTAL_SCORE_CALCULATOR_HPP

#include <vector>

#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// ChainedIncrementalScoreCalculator
// ---------------------------------------------------------------------------
// The constraint interface for CHAINED solutions. A chained model's backward
// `previous` pointers cannot be iterated forward cheaply — upstream solves
// that with auto shadow variables (anchor, nextEntity); here the engine hands
// the affected chain's CONTENT (anchor-ordered entity ids) straight into the
// hooks instead, which is what those shadows would be used to reconstruct:
//
//   beforeChainChanged(domain, anchor, chain)  // chain = OLD contents: retract
//   <the engine rewires the previous pointers>
//   afterChainChanged(domain, anchor, chain)   // chain = NEW contents: insert
//
// resetWorkingSolution receives all chains. The usual incremental contract
// applies: after any hook sequence, calculateScore() must equal a full
// recompute (checked under EnvironmentMode::FULL_ASSERT).
template <typename Solution>
class ChainedIncrementalScoreCalculator {
public:
    virtual ~ChainedIncrementalScoreCalculator() = default;

    virtual void resetWorkingSolution(Solution& workingSolution,
                                      const std::vector<std::vector<int>>& chains) = 0;
    virtual void beforeChainChanged(Solution& /*workingSolution*/, int /*anchorIndex*/,
                                    const std::vector<int>& /*chain*/) {}
    virtual void afterChainChanged(Solution& /*workingSolution*/, int /*anchorIndex*/,
                                   const std::vector<int>& /*chain*/) {}
    virtual HardSoftScore calculateScore() = 0;
};

}  // namespace optimaru

#endif  // OPTIMARU_API_SCORE_CALCULATOR_CHAINED_INCREMENTAL_SCORE_CALCULATOR_HPP

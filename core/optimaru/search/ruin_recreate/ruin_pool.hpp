// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_RUIN_RECREATE_RUIN_POOL_HPP
#define OPTIMARU_SEARCH_RUIN_RECREATE_RUIN_POOL_HPP

#include <memory>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/search/operators.hpp"
#include "optimaru/search/ruin_recreate/random_ruin.hpp"
#include "optimaru/search/ruin_recreate/related_ruin.hpp"
#include "optimaru/search/ruin_recreate/route_ruin.hpp"
#include "optimaru/search/ruin_recreate/sisr_ruin.hpp"
#include "optimaru/search/ruin_recreate/worst_ruin.hpp"
#include "optimaru/search/select/schemes.hpp"
#include "optimaru/settings.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// RuinPool
// ---------------------------------------------------------------------------
// The ruin operators of one solve and the adaptive selection over them. The
// built-in ruins are plugins like any other: one file each in this directory,
// registered ahead of the caller's operators (Solver::add_ruin_operator) so
// the selector sees a single uniform list. The stagnation kick (kick.hpp) and
// the destroy-repair move (ruin_recreate_move.hpp) draw from the pool through
// selector() and report their outcomes to it.
template <typename Solution>
class RuinPool {
    using Traits = SolutionTraits<Solution>;

public:
    RuinPool(const AlnsSettings& lns,
             const std::vector<std::unique_ptr<RuinOperator<Solution>>>& custom)
        : sisr_(lns.sisr_max_string_removals, lns.sisr_max_string_size),
          operators_{&random_, &related_, &worst_} {
        // sequence shape only — and not merely as an optimization. Both delegate to
        // the random ruin on assignment solutions (a "substring" and a "route" mean
        // nothing without an ordered list), so registering them there would put
        // three copies of the random ruin in a five-operator pool and silently
        // triple its selection weight for every assignment use case.
        if constexpr (Traits::shape == Shape::Sequence) {
            if (lns.sisr_ruin) operators_.push_back(&sisr_);
            if (lns.route_ruin) operators_.push_back(&route_);
        }
        for (const auto& op : custom) operators_.push_back(op.get());
        selector_ = make_operator_selection_scheme(lns, operators_.size());
    }

    // operators_ points at the members.
    RuinPool(const RuinPool&) = delete;
    RuinPool& operator=(const RuinPool&) = delete;

    const std::vector<RuinOperator<Solution>*>& operators() const { return operators_; }
    OperatorSelectionScheme& selector() { return *selector_; }

private:
    RandomRuin<Solution> random_;
    RelatedRuin<Solution> related_;
    WorstRuin<Solution> worst_;
    SisrRuin<Solution> sisr_;
    RouteRuin<Solution> route_;
    std::vector<RuinOperator<Solution>*> operators_;
    std::unique_ptr<OperatorSelectionScheme> selector_;
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_RUIN_RECREATE_RUIN_POOL_HPP

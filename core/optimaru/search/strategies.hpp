// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_STRATEGIES_HPP
#define OPTIMARU_SEARCH_STRATEGIES_HPP

#include <cstddef>
#include <memory>
#include <random>
#include <vector>

#include "optimaru/model/element_index.hpp"
#include "optimaru/model/neighborhoods.hpp"
#include "optimaru/model/solution_traits.hpp"
#include "optimaru/search/moves/descent.hpp"
#include "optimaru/search/moves/ejection_chain.hpp"
#include "optimaru/search/moves/granular_local_search.hpp"
#include "optimaru/search/moves/iterated_local_search.hpp"
#include "optimaru/search/moves/route_elimination.hpp"
#include "optimaru/search/operators.hpp"
#include "optimaru/search/ruin_recreate/greedy_repair.hpp"
#include "optimaru/search/ruin_recreate/ruin_pool.hpp"
#include "optimaru/search/ruin_recreate/ruin_recreate_move.hpp"
#include "optimaru/select/schemes.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/utils/random.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// BuiltinStrategies
// ---------------------------------------------------------------------------
// The built-in strategies of one solve (StrategySettings): route elimination,
// the ejection chain, the iterated local search, descent and destroy-repair
// as a move. Each is a plugin like any other MoveOperator, in moves/ or
// ruin_recreate/, drawn by weight before the ordinary move mix gets its turn
// (draw()). Their weights are small by necessity — each costs orders of
// magnitude more than one sampled move — and the header of StrategySettings
// records why that is a configured number here rather than something the
// solver's adaptive selector is asked to discover.
template <typename Solution>
class BuiltinStrategies {
    using Traits = SolutionTraits<Solution>;

public:
    BuiltinStrategies(const Settings& config, const Neighborhoods& nearby,
                      ElementIndex<Solution>& index, GreedyRepair<Solution>& repair,
                      RuinPool<Solution>& ruins)
        : route_elimination_(index, repair, config.strategies.route_elimination_max_size),
          descent_(index, nearby, config.strategies.descent_max_sweeps,
                   config.selection.entity_hint, config.strategies.granular_descent,
                   config.strategies.granular_descent_max_segment,
                   config.strategies.granular_descent_candidate_routes, config.unassigned_list),
          ruin_recreate_move_(index, repair, ruins.operators(), ruins.selector(),
                              config.strategies.ruin_recreate_max_ruin),
          ejection_chain_(index, nearby, config.unassigned_list,
                          config.strategies.ejection_chain_max_steps,
                          config.strategies.ejection_chain_candidate_lists,
                          config.strategies.ejection_chain_max_evaluations),
          granular_local_search_(index, nearby,
                                 config.strategies.iterated_local_search_max_segment),
          ils_ruin_selector_(make_operator_selection_scheme(config.alns, ruins.operators().size())),
          iterated_local_search_(
                  index, repair, granular_local_search_, ruins.operators(), *ils_ruin_selector_,
                  config.strategies.iterated_local_search_min_ruin,
                  config.strategies.iterated_local_search_max_ruin, config.unassigned_list,
                  config.strategies.iterated_local_search_max_evaluations) {
        const StrategySettings& st = config.strategies;
        // Route elimination is sequence-only, for the same reason the SISR and
        // route ruins are: it declines on assignment solutions, so registering it
        // there would only burn steps on an operator that can never move.
        // Descent has a neighbourhood on both shapes and is registered on both.
        if constexpr (Traits::shape == Shape::Sequence) {
            if (st.route_elimination_weight > 0.0) {
                weighted_.push_back({st.route_elimination_weight, &route_elimination_});
            }
            if (st.ejection_chain_weight > 0.0) {
                weighted_.push_back({st.ejection_chain_weight, &ejection_chain_});
            }
            if (st.iterated_local_search_weight > 0.0) {
                weighted_.push_back({st.iterated_local_search_weight, &iterated_local_search_});
            }
        }
        if (st.descent_weight > 0.0) {
            weighted_.push_back({st.descent_weight, &descent_});
        }
        if (st.ruin_recreate_weight > 0.0) {
            weighted_.push_back({st.ruin_recreate_weight, &ruin_recreate_move_});
        }
    }

    // weighted_ points at the members.
    BuiltinStrategies(const BuiltinStrategies&) = delete;
    BuiltinStrategies& operator=(const BuiltinStrategies&) = delete;

    // How many strategies this solve draws among.
    std::size_t size() const { return weighted_.size(); }

    // Offer each strategy its own draw, in priority order, and take the first
    // that hits; null means this step falls through to the ordinary move mix.
    //
    // The draws are sequential, so a weight is the probability of firing GIVEN
    // that everything before it missed, not a share of all steps: at the
    // vehicle-routing defaults route elimination runs on 0.10 of steps, the
    // ejection chain on 0.9 x 0.05, the iterated local search on
    // 0.9 x 0.95 x 0.05, descent on 0.9 x 0.95 x 0.95 x 0.03 and
    // destroy-repair on 0.9 x 0.95 x 0.95 x 0.97 x 0.05. The weights are small
    // enough that the difference barely shows, and they were measured through
    // this same code so the numbers are self-consistent — but the ORDER in
    // the constructor is a priority, and raising a weight dilutes everything
    // after it.
    MoveOperator<Solution>* draw(std::mt19937_64& rng) {
        for (const Weighted& s : weighted_) {
            if (random_unit(rng) < s.weight) return s.op;
        }
        return nullptr;
    }

private:
    // A strategy and the probability a step draws it.
    struct Weighted {
        double weight;
        MoveOperator<Solution>* op;
    };

    RouteElimination<Solution> route_elimination_;
    Descent<Solution> descent_;
    RuinRecreateMove<Solution> ruin_recreate_move_;
    EjectionChain<Solution> ejection_chain_;
    GranularLocalSearch<Solution> granular_local_search_;
    // Its own adaptive ruin weights: an ILS outcome is measured after the
    // descent and would otherwise retrain the destroy-repair move's.
    std::unique_ptr<OperatorSelectionScheme> ils_ruin_selector_;
    IteratedLocalSearchMove<Solution> iterated_local_search_;
    std::vector<Weighted> weighted_;
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_STRATEGIES_HPP

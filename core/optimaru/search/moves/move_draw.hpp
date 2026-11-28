// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_MOVES_MOVE_DRAW_HPP
#define OPTIMARU_SEARCH_MOVES_MOVE_DRAW_HPP

#include <random>
#include <vector>

#include "optimaru/model/element_index.hpp"
#include "optimaru/model/neighborhoods.hpp"
#include "optimaru/search/moves/list_move.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/utils/random.hpp"

namespace optimaru {

// One move of the mix, with what is needed to reject it and to feed the
// acceptor's tabu memory.
struct SampledMove {
    int entities[2] = {0, 0};  // the entities / elements the move is about
    int entity_count = 0;
    int previous[2] = {0, 0};  // assignment: the value each entity held before
    ListMove list;             // sequence: the move itself
};

// What every move of the mix draws with: the settings, the nearby lists and,
// in the sequence shape, the solve's element index. One per MoveMixSampler,
// so all its moves share the same draws in the same order.
template <typename Solution>
struct MoveDraw {
    const Settings& config;
    const Neighborhoods& nearby;
    ElementIndex<Solution>& index;
    std::uniform_real_distribution<double> unit{0.0, 1.0};

    // A random nearby neighbour of `e`, drawn with NeighborSettings::probability
    // when nearby selection is configured; -1 means draw uniformly instead.
    int nearby_pick(int e, std::mt19937_64& rng) {
        if (!nearby.enabled() || !(unit(rng) < config.selection.nearby.probability)) return -1;
        const std::vector<int>& neighbours = nearby.of(e);
        return neighbours.empty() ? -1 : neighbours[rng() % neighbours.size()];
    }
};

// Uniformly draw from [0, n) except one value, without rejection. This is
// used by moves where drawing the current entity/position is a guaranteed
// no-op. Besides wasting a score evaluation, accepted no-ops advance Late
// Acceptance history and dilute the useful neighbourhood.
inline int random_other(int n, int excluded, std::mt19937_64& rng) {
    int picked = random_index(rng, n - 1);
    if (picked >= excluded) ++picked;
    return picked;
}

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_MOVES_MOVE_DRAW_HPP

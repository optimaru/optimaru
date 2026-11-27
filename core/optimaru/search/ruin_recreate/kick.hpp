// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_RUIN_RECREATE_KICK_HPP
#define OPTIMARU_SEARCH_RUIN_RECREATE_KICK_HPP

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "optimaru/model/editor.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/search/moves/move_mix.hpp"
#include "optimaru/search/operators.hpp"
#include "optimaru/search/ruin_recreate/greedy_repair.hpp"
#include "optimaru/search/ruin_recreate/removal_gain.hpp"
#include "optimaru/search/ruin_recreate/ruin_pool.hpp"
#include "optimaru/search/select/scheme.hpp"
#include "optimaru/settings.hpp"

namespace optimaru {

// How many of its `n` candidates a kick ruins: `drawn`, its share of them,
// kept to at most n - 1 and the configured maximum, and to at least one. One
// also where n - 1 or the maximum is below one (a single entity or element):
// a kick that ruins nothing is no kick.
inline int kick_ruin_count(int drawn, int n, int max_ruin_count) {
    return std::clamp(drawn, 1, std::max(1, std::min(n - 1, max_ruin_count)));
}

// ---------------------------------------------------------------------------
// StagnationKick — adaptive ruin & recreate (the ALNS iteration)
// ---------------------------------------------------------------------------
// What the solver does when the search has stalled (AlnsTriggerSettings): one
// large ruin drawn from the pool, the greedy / regret-k recreate, and a short
// hill-climbing polish with the move mix. The result becomes the new
// incumbent unconditionally (perturbation-style); outcomes are still
// classified to adapt the ruin-operator weights. When a kick is due is the
// solver's to decide (Solver::run).
template <typename Solution>
class StagnationKick {
public:
    StagnationKick(const AlnsSettings& lns, GreedyRepair<Solution>& repair,
                   RuinPool<Solution>& ruins, MoveMixSampler<Solution>& moves)
        : lns_(lns), repair_(repair), ruins_(ruins), moves_(moves) {}

    // One kick; returns the score of the solution it leaves in place.
    Score apply(OperatorContext<Solution>& ctx, const Score& current_score, const Score& best_score,
                std::mt19937_64& rng) {
        Editor<Solution>& editor = ctx.editor;
        const int n = ruin_candidate_count(ctx);
        std::uniform_real_distribution<double> fraction(lns_.min_ruin_fraction,
                                                        lns_.max_ruin_fraction);
        const int ruin_count =
                kick_ruin_count(static_cast<int>(fraction(rng) * n), n, lns_.max_ruin_count);

        int op = ruins_.selector().select(rng);
        std::vector<int> ruined = ruins_.operators()[op]->select_ruin_set(ctx, ruin_count, rng);
        // Defensive: a ruin set must be duplicate-free (the built-ins are by
        // construction; a custom operator is not required to be) and must not
        // contain a pinned entity. dedupe() is order-preserving, which is
        // load-bearing — see its comment.
        repair_.dedupe(editor, ruined, n);
        repair_.ruin(editor, ruined);
        repair_.recreate(editor, ruined, rng);

        // Polish the recreated solution with hill-climbing small moves so the
        // outcome classification below reflects the kick's real potential.
        Score candidate = editor.score();
        for (std::int64_t i = 0; i < lns_.polish_steps; ++i) {
            SampledMove move;
            Score polished;
            const bool previewed = moves_.sample(editor, rng, move, polished);
            if (polished >= candidate) {
                if (previewed) moves_.apply_previewed(editor, move);
                candidate = polished;
            } else if (!previewed) {
                moves_.undo(editor, move);
            }
        }
        // The kick is kept whatever it scored; the outcome only adapts the
        // ruin weights ("accepted" = it held the current score).
        const MoveOutcome outcome =
                classify_outcome(candidate, current_score, best_score, candidate == current_score);
        ruins_.selector().update(op, outcome);
        return candidate;
    }

private:
    const AlnsSettings& lns_;
    GreedyRepair<Solution>& repair_;
    RuinPool<Solution>& ruins_;
    MoveMixSampler<Solution>& moves_;
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_RUIN_RECREATE_KICK_HPP

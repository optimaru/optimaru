// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_RUIN_RECREATE_MOVE_HPP
#define OPTIMARU_SEARCH_RUIN_RECREATE_MOVE_HPP

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/select/scheme.hpp"
#include "optimaru/model/element_index.hpp"
#include "optimaru/repair/greedy_repair.hpp"
#include "optimaru/model/snapshot.hpp"
#include "optimaru/search/operators.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Destroy-and-repair as an ORDINARY move
// ---------------------------------------------------------------------------
// The same ruin-and-recreate local search fires as a stagnation kick, but entered
// as a normal candidate move: a small ruin is applied, repaired, scored, and
// UNDONE if the acceptor rejects it.
//
// Destroy-and-repair is treated as an ordinary candidate, judged by the normal
// acceptance criterion. A stagnation kick is instead rare and unconditional.
//
// The ruin set comes from the same adaptive pool the kick draws from, and the
// outcome is reported back to it, so a ruin operator is credited for what it
// achieves here too and not only for what it achieves in a kick.
//
// Works for both variable kinds: on assignment the repair is plain best-value.
template <typename Solution>
class RuinRecreateMove final : public MoveOperator<Solution> {
    using Traits = SolutionTraits<Solution>;

public:
    RuinRecreateMove(ElementIndex<Solution>& index, GreedyRepair<Solution>& repair,
                     const std::vector<RuinOperator<Solution>*>& ruins,
                     OperatorSelectionScheme& ruin_selector, int max_ruin)
        : index_(index),
          repair_(repair),
          ruins_(ruins),
          ruin_selector_(ruin_selector),
          max_ruin_(max_ruin) {}

    const char* name() const override { return "ruinRecreateMove"; }

    bool apply(OperatorContext<Solution>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        Editor<Solution>& editor = ctx.editor;
        const int n = GreedyRepair<Solution>::candidate_count(editor.solution());
        if (n < 2 || ruins_.empty()) return false;

        // Much smaller than a kick's ruin: it has to be cheap enough to run
        // every few steps, and small enough that the acceptor sees a local
        // change rather than a restart.
        const int ruin_count = std::clamp(
                1 + static_cast<int>(rng() % static_cast<std::uint64_t>(std::max(1, max_ruin_))),
                1, n - 1);

        drawn_ruin_ = ruin_selector_.select(rng);
        std::vector<int> ruined = ruins_[drawn_ruin_]->select_ruin_set(ctx, ruin_count, rng);
        repair_.dedupe(editor, ruined, n);
        if (ruined.empty()) return false;

        // Snapshot only once the move is known to happen — it is O(n) ints,
        // negligible next to the score evaluations the repair itself performs,
        // but there is no reason to pay it for a move that never starts.
        SolutionSnapshot<Solution>::capture(before_, editor.solution());
        repair_.ruin(editor, ruined);
        repair_.recreate(editor, ruined, rng);
        entities = std::move(ruined);
        return true;
    }

    void undo_last(OperatorContext<Solution>& ctx) override {
        SolutionSnapshot<Solution>::restore(ctx.editor, before_);
        if constexpr (Traits::shape == Shape::Sequence) {
            // restore() writes past the editor's list hooks, so touched-list
            // resynchronization cannot see it: rebuild the index outright.
            index_.build(ctx.editor.solution());
        }
    }

    // Credit the ruin operator that was drawn, on the same four-outcome scale
    // the kick uses.
    void outcome(MoveOutcome outcome) override {
        if (drawn_ruin_ >= 0) ruin_selector_.update(drawn_ruin_, outcome);
    }

private:
    ElementIndex<Solution>& index_;
    GreedyRepair<Solution>& repair_;
    const std::vector<RuinOperator<Solution>*>& ruins_;
    OperatorSelectionScheme& ruin_selector_;
    int max_ruin_;
    int drawn_ruin_ = -1;
    SolutionSnapshot<Solution> before_;
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_RUIN_RECREATE_MOVE_HPP

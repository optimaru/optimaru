#ifndef OPTIMARU_IMPL_LOCALSEARCH_STRATEGY_RUIN_RECREATE_MOVE_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_STRATEGY_RUIN_RECREATE_MOVE_HPP

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/select/operator_selection_scheme.hpp"
#include "optimaru/impl/localsearch/element_index.hpp"
#include "optimaru/impl/localsearch/greedy_repair.hpp"
#include "optimaru/impl/localsearch/solution_snapshot.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Destroy-and-repair as an ORDINARY move
// ---------------------------------------------------------------------------
// The same ruin-and-recreate the phase fires as a stagnation kick, but entered
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
// Works for both variable kinds: on BASIC the repair is plain best-value.
template <typename Solution>
class RuinRecreateMove final : public MoveOperator<Solution> {
    using Descriptor = SolutionDescriptor<Solution>;

public:
    RuinRecreateMove(ElementIndex<Solution>& index, GreedyRepair<Solution>& repair,
                     const std::vector<RuinOperator<Solution>*>& ruins,
                     OperatorSelectionScheme& ruinSelector, int maxRuin)
        : index_(index),
          repair_(repair),
          ruins_(ruins),
          ruinSelector_(ruinSelector),
          maxRuin_(maxRuin) {}

    const char* name() const override { return "ruinRecreateMove"; }

    bool apply(OperatorContext<Solution>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        ScoreDirector<Solution>& director = ctx.director;
        const int n = GreedyRepair<Solution>::candidateCount(director.workingSolution());
        if (n < 2 || ruins_.empty()) return false;

        // Much smaller than a kick's ruin: it has to be cheap enough to run
        // every few steps, and small enough that the acceptor sees a local
        // change rather than a restart.
        const int ruinCount = std::clamp(
                1 + static_cast<int>(rng() % static_cast<std::uint64_t>(std::max(1, maxRuin_))),
                1, n - 1);

        drawnRuin_ = ruinSelector_.select(rng);
        std::vector<int> ruined = ruins_[drawnRuin_]->selectRuinSet(ctx, ruinCount, rng);
        repair_.dedupe(director, ruined, n);
        if (ruined.empty()) return false;

        // Snapshot only once the move is known to happen — it is O(n) ints,
        // negligible next to the score evaluations the repair itself performs,
        // but there is no reason to pay it for a move that never starts.
        SolutionSnapshot<Solution>::capture(before_, director.workingSolution());
        repair_.ruin(director, ruined);
        repair_.recreate(director, ruined, rng);
        entities = std::move(ruined);
        return true;
    }

    void undoLast(OperatorContext<Solution>& ctx) override {
        SolutionSnapshot<Solution>::restore(ctx.director, before_);
        if constexpr (Descriptor::kind == VariableKind::LIST) {
            // restore() writes past the director's list hooks, so touched-list
            // resynchronization cannot see it: rebuild the index outright.
            index_.build(ctx.director.workingSolution());
        }
    }

    // Credit the ruin operator that was drawn, on the same four-outcome scale
    // the kick uses.
    void outcome(LnsOutcome outcome) override {
        if (drawnRuin_ >= 0) ruinSelector_.update(drawnRuin_, outcome);
    }

private:
    ElementIndex<Solution>& index_;
    GreedyRepair<Solution>& repair_;
    const std::vector<RuinOperator<Solution>*>& ruins_;
    OperatorSelectionScheme& ruinSelector_;
    int maxRuin_;
    int drawnRuin_ = -1;
    SolutionSnapshot<Solution> before_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_STRATEGY_RUIN_RECREATE_MOVE_HPP

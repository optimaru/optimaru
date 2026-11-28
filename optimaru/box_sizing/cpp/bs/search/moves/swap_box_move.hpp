#ifndef BS_SEARCH_MOVES_SWAP_BOX_MOVE_HPP
#define BS_SEARCH_MOVES_SWAP_BOX_MOVE_HPP

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "bs/search/moves/open_set_move.hpp"

namespace bs {

// SwapBoxMove: open a size that is not in use and close one that is, in one
// step. At maxBoxTypes this is the only way to another set of sizes: opening
// alone breaks the limit, and closing alone gives up a size the parcels need.
//
// The size to open is drawn uniformly among those not in use. The size to
// close is the one whose closing costs the least once the new size is open,
// and one time in kRandomClose a uniformly drawn one, so that a walk below the
// best (bs/accept/reheating_late_acceptance.hpp) is not always the same one.
//
// The cheapest size to close takes one pass over the groups of parcels, for
// all sizes at once (the interchange of the p-median heuristics): a group that
// the new size serves cheaper moves there and costs its size nothing; any
// other group costs the size it is in what it pays more in the next cheapest
// size that takes it, the new one included. A group no other size of the set
// takes makes its size as good as impossible to close.
//
// The loss of a group is charged to its cheapest size in use, which is where
// the last move on the set put it. A parcel that a change move or a seed left
// in another size is counted as if it were there too: the choice is a guess
// at what to propose, and the acceptor judges the trade by the real score.
class SwapBoxMove final : public OpenSetMove {
public:
    static constexpr int kRandomClose = 4;

    using OpenSetMove::OpenSetMove;

    const char* name() const override { return "swapBox"; }

    bool apply(optimaru::OperatorContext<BoxSizingProblem>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        const BoxSizingProblem& problem = ctx.editor.solution();
        census(problem);
        if (closed_.empty() || closable_.empty()) return false;
        const int opening = closed_[optimaru::random_index(rng, static_cast<int>(closed_.size()))];
        int closing = closable_[optimaru::random_index(rng, static_cast<int>(closable_.size()))];
        if (closable_.size() > 1 && optimaru::random_index(rng, kRandomClose) != 0) {
            closing = cheapestToClose(problem, opening);
        }
        planned_[opening] = 1;
        planned_[closing] = 0;
        return commit(ctx, entities, closing);
    }

private:
    // The size in use (not required) whose parcels pay the least more when it
    // closes and `opening` opens.
    int cheapestToClose(const BoxSizingProblem& problem, int opening) {
        inUse_.clear();
        for (int box : ranking_->order) {
            if (planned_[box]) inUse_.push_back(box);
        }
        loss_.assign(planned_.size(), 0.0);
        const bool perGroup = ranking_->perGroup;
        for (std::size_t g = 0; g < ranking_->groups.size(); ++g) {
            const int group = static_cast<int>(g);
            const int p = ranking_->groups[g].front();  // what takes it takes its group
            // The two cheapest sizes in use that take the group.
            int first = -1;
            int second = -1;
            // With shipping prices the group's own order, which holds only
            // the boxes that take it; without, the sizes in use.
            for (int box : perGroup ? ranking_->groupOrder[g] : inUse_) {
                if (perGroup ? !planned_[box] : !problem.fits(p, box)) continue;
                if (first < 0) {
                    first = box;
                } else {
                    second = box;
                    break;
                }
            }
            if (first < 0) continue;  // nothing in use takes it: it stays where it is
            const bool takes = problem.fits(p, opening);
            if (takes && ranking_->rankOf(group, opening) < ranking_->rankOf(group, first)) {
                continue;  // it moves to the new size
            }
            double next = std::numeric_limits<double>::infinity();
            if (second >= 0) next = ranking_->priceOf(group, second);
            if (takes) next = std::min(next, ranking_->priceOf(group, opening));
            loss_[first] += static_cast<double>(ranking_->groupQuantity[g]) *
                            (next - ranking_->priceOf(group, first));
        }
        // Every size may be as good as impossible to close (an infinite
        // loss): the first one is then returned, and commit() brings in the
        // size its parcels need or does not make the move.
        int best = closable_.front();
        for (int box : closable_) {
            if (loss_[box] < loss_[best]) best = box;
        }
        return best;
    }

    std::vector<int> inUse_;    // the sizes in use, cheapest first
    std::vector<double> loss_;  // per size: what closing it costs its parcels
};

}  // namespace bs

#endif  // BS_SEARCH_MOVES_SWAP_BOX_MOVE_HPP

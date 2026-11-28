#ifndef BS_SEARCH_MOVES_OPEN_SET_MOVE_HPP
#define BS_SEARCH_MOVES_OPEN_SET_MOVE_HPP

#include <algorithm>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "bs/model/domain.hpp"
#include "bs/model/pricing.hpp"
#include "optimaru/search/operators.hpp"
#include "optimaru/utils/random.hpp"

namespace bs {

// ---------------------------------------------------------------------------
// The base of the three box-sizing moves
// ---------------------------------------------------------------------------
// Which sizes are in use is a step function of the assignment: a size stops
// counting only when its LAST parcel leaves, and a size worth opening pays
// off only once every parcel it serves cheaper has moved to it. A change move
// does one parcel of that, is worse on its own, and is turned down. So the
// search would keep the sizes the construction chose.
//
// The moves here decide on the set of sizes instead, and re-place the parcels
// for it in the same step: CloseBoxMove takes one size out of the set,
// OpenBoxMove puts one in, SwapBoxMove does both. The acceptor judges the
// whole trade with one score.
//
// Given the set, where a parcel goes needs no search: its cheapest box is the
// first box of the set, in the ranking's order, that takes it (BoxRanking).
// commit() therefore puts every parcel in its cheapest box of the planned set,
// which also undoes what single-parcel moves left elsewhere.
//
// Cost per move: one pass over the parcels for the census, and one over the
// groups of parcels that the same boxes take (BoxRanking::groups) for the
// re-placement, each group looking down the planned set until a box takes it.
class OpenSetMove : public optimaru::MoveOperator<BoxSizingProblem> {
public:
    explicit OpenSetMove(std::shared_ptr<const BoxRanking> ranking)
        : ranking_(std::move(ranking)) {}

    void undo_last(optimaru::OperatorContext<BoxSizingProblem>& ctx) override {
        ctx.editor.begin_batch();
        for (auto move = moved_.rbegin(); move != moved_.rend(); ++move) {
            ctx.editor.assign(move->first, move->second);
        }
        ctx.editor.end_batch();
    }

protected:
    // Reads the current solution: the parcels per box, the sizes in use (a
    // required one always is), and the plan, which starts as that set.
    void census(const BoxSizingProblem& problem) {
        const int boxCount = problem.boxCount();
        count_.assign(static_cast<std::size_t>(boxCount), 0);
        // No range check: a solve has been through ScoreCalculator::prepare,
        // which refuses a box outside -1..boxCount - 1.
        for (const Parcel& parcel : problem.parcels) {
            if (parcel.box >= 0) ++count_[parcel.box];
        }
        planned_.assign(static_cast<std::size_t>(boxCount), 0);
        closable_.clear();
        closed_.clear();
        types_ = 0;
        for (int b = 0; b < boxCount; ++b) {
            const bool required = problem.boxes[b].required;
            if (count_[b] > 0 || required) {
                planned_[b] = 1;
                ++types_;
                // Emptying a required size takes nothing out of use.
                if (count_[b] > 0 && !required) closable_.push_back(b);
            } else {
                closed_.push_back(b);
            }
        }
    }

    bool atLimit(const BoxSizingProblem& problem) const {
        return problem.maxBoxTypes && types_ >= *problem.maxBoxTypes;
    }

    // Puts every parcel in its cheapest box of the planned set. `closing` is
    // the size the plan takes out (-1 = none): a parcel of it that no planned
    // size takes brings the cheapest other size that does into the plan. The
    // move is not made (false, nothing written) when no other candidate takes
    // such a parcel, and when the sizes brought in would put more sizes in
    // the plan than maxBoxTypes allows and than are in use now: at the limit
    // a swap that needs a second new size could only break it. A parcel
    // elsewhere that no planned size takes stays where it is. False also when
    // the plan moves nothing.
    bool commit(optimaru::OperatorContext<BoxSizingProblem>& ctx, std::vector<int>& entities,
                int closing) {
        const BoxSizingProblem& problem = ctx.editor.solution();
        if (closing >= 0) {
            bool broughtIn = false;
            for (const std::vector<int>& group : ranking_->groups) {
                bool inClosing = false;
                for (int p : group) inClosing = inClosing || problem.parcels[p].box == closing;
                if (!inClosing || cheapestPlanned(problem, group.front()) >= 0) continue;
                const int other = ranking_->cheapestFor(problem, group.front(), closing);
                if (other < 0) return false;
                planned_[other] = 1;
                broughtIn = true;
            }
            if (broughtIn && problem.maxBoxTypes) {
                int plannedTypes = 0;
                for (char planned : planned_) plannedTypes += planned;
                if (plannedTypes > std::max(*problem.maxBoxTypes, types_)) return false;
            }
        }
        plannedOrder_.clear();
        for (int box : ranking_->order) {
            if (planned_[box]) plannedOrder_.push_back(box);
        }

        moved_.clear();
        ctx.editor.begin_batch();
        for (const std::vector<int>& group : ranking_->groups) {
            // The boxes that take one parcel of a group take them all.
            int to = -1;
            for (int box : plannedOrder_) {
                if (problem.fits(group.front(), box)) {
                    to = box;
                    break;
                }
            }
            if (to < 0) continue;
            for (int p : group) {
                const int from = problem.parcels[p].box;
                if (from < 0 || from == to) continue;  // -1: inside a ruin, that parcel is nowhere
                moved_.emplace_back(p, from);
                ctx.editor.assign(p, to);
                entities.push_back(p);
            }
        }
        ctx.editor.end_batch();
        return !moved_.empty();
    }

    std::shared_ptr<const BoxRanking> ranking_;

    // Scratch, reused across moves (no per-move allocation).
    std::vector<int> count_;     // parcels per box
    std::vector<char> planned_;  // per box: in the planned set
    std::vector<int> closable_;  // sizes in use that are not required
    std::vector<int> closed_;    // sizes not in use
    int types_ = 0;              // sizes in use, the required ones included

private:
    int cheapestPlanned(const BoxSizingProblem& problem, int parcel) const {
        for (int box : ranking_->order) {
            if (planned_[box] && problem.fits(parcel, box)) return box;
        }
        return -1;
    }

    std::vector<int> plannedOrder_;           // the planned set, cheapest first
    std::vector<std::pair<int, int>> moved_;  // (parcel, box before the move)
};

}  // namespace bs

#endif  // BS_SEARCH_MOVES_OPEN_SET_MOVE_HPP

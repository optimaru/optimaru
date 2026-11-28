#ifndef BS_SEARCH_MOVES_CLOSE_BOX_MOVE_HPP
#define BS_SEARCH_MOVES_CLOSE_BOX_MOVE_HPP

#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "bs/search/moves/open_set_move.hpp"

namespace bs {

// CloseBoxMove: take one size out of use and send each of its parcels to the
// cheapest remaining size that takes it. The move that brings a solution with
// too many sizes under maxBoxTypes, and, with a fixed cost per size
// (boxTypeCost), the one that drops a size that does not pay for itself.
//
// The size is drawn uniformly among those in use that are not required. No
// move when there is none, or when one of its parcels fits no other candidate.
class CloseBoxMove final : public OpenSetMove {
public:
    using OpenSetMove::OpenSetMove;

    const char* name() const override { return "closeBox"; }

    bool apply(optimaru::OperatorContext<BoxSizingProblem>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        census(ctx.editor.solution());
        if (closable_.empty()) return false;
        const int closing =
                closable_[optimaru::random_index(rng, static_cast<int>(closable_.size()))];
        planned_[closing] = 0;
        return commit(ctx, entities, closing);
    }
};

}  // namespace bs

#endif  // BS_SEARCH_MOVES_CLOSE_BOX_MOVE_HPP

#ifndef BS_SEARCH_MOVES_OPEN_BOX_MOVE_HPP
#define BS_SEARCH_MOVES_OPEN_BOX_MOVE_HPP

#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "bs/search/moves/open_set_move.hpp"

namespace bs {

// OpenBoxMove: put one more size into use and pull in every parcel it serves
// cheaper than the size the parcel is in. The move that uses room under
// maxBoxTypes the search has left, for instance after a CloseBoxMove.
//
// The size is drawn uniformly among those not in use. No move when all are in
// use, when the limit is reached (one more size would only break it:
// SwapBoxMove is the move then), or when no parcel would move.
class OpenBoxMove final : public OpenSetMove {
public:
    using OpenSetMove::OpenSetMove;

    const char* name() const override { return "openBox"; }

    bool apply(optimaru::OperatorContext<BoxSizingProblem>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        const BoxSizingProblem& problem = ctx.editor.solution();
        census(problem);
        if (closed_.empty() || atLimit(problem)) return false;
        planned_[closed_[optimaru::random_index(rng, static_cast<int>(closed_.size()))]] = 1;
        return commit(ctx, entities, -1);
    }
};

}  // namespace bs

#endif  // BS_SEARCH_MOVES_OPEN_BOX_MOVE_HPP

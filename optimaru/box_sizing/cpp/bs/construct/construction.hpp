#ifndef BS_CONSTRUCTION_HPP
#define BS_CONSTRUCTION_HPP

#include "bs/model/domain.hpp"
#include "bs/model/pricing.hpp"

namespace bs {

// ---------------------------------------------------------------------------
// Domain construction (runs before the engine)
// ---------------------------------------------------------------------------
// The engine's construction heuristic places one parcel at a time in the box
// that scores best for it, which opens a size for nearly every parcel shape
// and leaves the limit to the search. This greedy decides the sizes first:
//
//   1. the open sizes start as the required boxes and those a seed ships in;
//   2. while a parcel is taken by no open size, it opens the size that takes
//      the most such parcels (ties: the cheaper size);
//   3. then it opens, one at a time, the size with the largest saving: what
//      the parcels that would move to it save, less the fixed cost of a size
//      (Pricing::typeWeight). It stops at maxBoxTypes or when no size saves
//      anything;
//   4. every parcel still at -1 goes to the cheapest open size that takes it.
//
// Savings only shrink as sizes open, so step 3 re-evaluates a size only when
// it comes to the top of the queue (lazy greedy): the same picks as
// evaluating every size every round.
//
// A parcel that already has a box (a seed) stays put, and every size a seed
// ships in is open, also when the seed uses more sizes than maxBoxTypes. When
// maxBoxTypes is reached with parcels no open size takes (at once, with such
// a seed), step 4 puts each in the cheapest size that takes it and leaves the
// limit broken for the search to repair: CloseBoxMove is the move that does.
// A parcel no candidate takes at all, which a solve refuses before it gets
// here, goes where it adds the least to the hard score. Requires
// ScoreCalculator::prepare. Deterministic: no random draw.
void constructGreedy(BoxSizingProblem& problem, const BoxRanking& ranking);

}  // namespace bs

#endif  // BS_CONSTRUCTION_HPP

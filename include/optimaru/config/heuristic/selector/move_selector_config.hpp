#ifndef OPTIMARU_CONFIG_HEURISTIC_SELECTOR_MOVE_SELECTOR_CONFIG_HPP
#define OPTIMARU_CONFIG_HEURISTIC_SELECTOR_MOVE_SELECTOR_CONFIG_HPP

namespace optimaru {

// Mirrors config.heuristic.selector.move (a unionMoveSelector with
// fixedProbabilityWeight per child): the built-in move mix, selected by
// relative weight each step. Weights need not sum to 1; a weight of 0
// disables the move. Moves that don't apply to the current solution shape
// (e.g. tail swap with a single list) are skipped automatically.
struct MoveSelectorConfig {
    // BASIC kind
    double changeMoveWeight = 0.75;   // reassign one entity
    double swapMoveWeight = 0.25;     // swap two entities' values

    // LIST kind
    double relocateMoveWeight = 0.5;  // move one element
    double listSwapMoveWeight = 0.3;  // swap two elements
    double twoOptMoveWeight = 0.2;    // reverse a segment within one list
    // Or-opt: relocate a short segment (2-3 consecutive elements) intact.
    double orOptMoveWeight = 0.0;
    // 2-opt* (tail swap): exchange the tails of two lists at random cut
    // points — the classic inter-route VRP move. Needs >= 2 lists.
    double tailSwapMoveWeight = 0.0;
    // CROSS-exchange: swap a run of elements in one list with a run in
    // another (the runs may differ in length, and one may be empty, which
    // makes it a segment relocate). InterExchangeRoute registers it at every length pair from 1x1 to
    // 3x3 and is the move that rebalances load between routes.
    //
    // Why the other inter-list moves do not cover it: the tail swap can only
    // cut once per list, so it cannot move work out of the MIDDLE of a route,
    // and relocate/or-opt move elements one way only, so shifting work from a
    // full route to a full one has to pass through an overloaded intermediate
    // state that a hard capacity constraint rejects. Exchanging both ways at
    // once keeps every intermediate feasible.
    double interExchangeMoveWeight = 0.0;
    // Longest run CROSS-exchange may take from either side.
    int interExchangeMaxSegment = 3;
};

}  // namespace optimaru

#endif  // OPTIMARU_CONFIG_HEURISTIC_SELECTOR_MOVE_SELECTOR_CONFIG_HPP

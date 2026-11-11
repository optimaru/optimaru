#ifndef BP_SOLVER_HPP
#define BP_SOLVER_HPP

#include <utility>

#include "bp/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"

namespace bp {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/:
// the domain construction (best-fit decreasing, bp/construction.hpp), then
// optimaru::Solver<BinPackingProblem> with late acceptance over the built-in
// change/swap moves, the domain's EmptyBinMove, RepackMove and TradeMove
// (bp/moves.hpp), ties between equal scores decided by how full the bins are
// (bp/fill_late_acceptance.hpp) and the engine's adaptive ruin-and-recreate
// (ALNS).
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   time_limit 0           no search: the construction (or the seed,
    //                          completed where an item is still -1) comes back
    //                          as it is
    //   history_length 0       clamp(8 * itemCount, 400, 3200)
    //   parallel_solver_count  start i searches its own construction: start 0
    //                          constructs in weight-descending order, start
    //                          i > 0 shuffles the items of equal weight (a seed
    //                          is completed the same way in every start)
    //   ruin_recreate          the recreate places each ruined item in its best
    //                          bin
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // Destroy-and-repair as an ORDINARY acceptor-judged move (the
        // engine's ruin_recreate.as_move_weight); one costs ~ruinCount * n score
        // evaluations. 0 disables it.
        double ruin_recreate_as_move_weight = 0.0;
        int ruin_recreate_as_move_max_ruin = 12;

        // Assignment move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Guided change-move targets (engine local_search.value_hint):
        // with this probability the change move draws the item's new bin as
        // the bin of a uniformly drawn item — a bin in use, weighted by how
        // many items it holds — instead of uniformly over all n bins, most of
        // which are empty in a good packing. 0 = off.
        double value_recommender_probability = 0.5;

        // Assignment best-improvement descent strategy (engine
        // local_search.descent_weight). 0 (default) = off.
        double descent_weight = 0.0;

        // Upper gate on how often a step may draw one of the domain's macro
        // moves (EmptyBinMove, RepackMove, TradeMove) instead of the ordinary mix; the
        // adaptive selector still chooses inside the gate. 1 = unrestricted
        // (engine default).
        double custom_move_probability = 1.0;

        // EmptyBinMove: empty one light bin in a single move (see
        // bp/moves.hpp). usedBinsCost is a step function no single-item move
        // can cross, so without it the search rarely closes a bin.
        bool empty_bin_moves = true;
        // EmptyBinMove and RepackMove empty one of this many lightest
        // non-empty bins (drawn uniformly; 1 = always the lightest), and
        // TradeMove takes its first bin from them every second time.
        int empty_bin_candidates = 3;

        // RepackMove (bp/moves.hpp): empty a light bin together with 1 to
        // repack_partner_bins other light bins and put their items back best
        // fit decreasing, only where they add no capacity, colour or
        // conflict violation. EmptyBinMove checks the capacity only, so on
        // instances with colours or conflicts this is the move that closes
        // bins. repack_partner_bins 0 = the source bin alone.
        bool repack_moves = true;
        int repack_partner_bins = 2;

        // TradeMove (bp/moves.hpp): trade up to two items of one bin for up
        // to two of another so that the fuller of the two gets fuller, or,
        // when no trade does that, so that the two keep their loads with
        // other items. usedBinsCost counts bins only, so this is the move
        // that works towards closing a bin while the score stands still.
        bool trade_moves = true;

        // Late acceptance takes a step that keeps the score when it does not
        // lower the sum of the squared bin loads, and otherwise one time in
        // eight (bp/fill_late_acceptance.hpp); false = it takes every such
        // step, as plain late acceptance does. Runs that end above the
        // capacity bound on Falkenauer's 24 sampled instances, two seeds
        // each, 10 s (benchmarks/bin-packing, 2026-11-12):
        //   both on               0 of 48
        //   trade_moves only      7 (249 and 501 items, triplets)
        //   fill_tie_break only  40
        //   both off             39
        bool fill_tie_break = true;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: sets items[*].bin and problem.score, clears
    // warmStart. A plain solve (warmStart false) ignores the input bins; a
    // warm start keeps them and completes the items still at -1. When the
    // search moved anything, the bins are relabelled 0..k-1 by first
    // appearance (score-neutral), so a complete seed with a zero budget
    // comes back exactly as it was, gaps included.
    // Throws std::invalid_argument for binCapacity <= 0, a repeated item
    // id, a weight <= 0, a bad conflict or (warm start) a bin outside
    // -1..n-1.
    void solve(BinPackingProblem& problem);

    // Solves with a caller-assembled plugin calculator (see
    // bp/constraints.hpp): the same construction and search, scored by the
    // assembly (custom weights, levels and exponents included). The final
    // problem.score is computed by THIS calculator and the calculator is left
    // primed on the final solution, so constraint_totals() explains exactly
    // that score. Single-start: the caller-owned calculator cannot be
    // cloned, so parallel_solver_count is ignored.
    void solve(BinPackingProblem& problem,
               optimaru::DecomposedObjective<BinPackingProblem>& calculator);

private:
    Config config_;
};

}  // namespace bp

#endif  // BP_SOLVER_HPP

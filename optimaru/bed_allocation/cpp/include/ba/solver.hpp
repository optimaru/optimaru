#ifndef BA_SOLVER_HPP
#define BA_SOLVER_HPP

#include <utility>

#include "ba/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"

namespace ba {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<BedPlan>, with the engine's adaptive ruin-and-recreate
// (ALNS) kicks enabled by default.
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   history_length 0  clamp(8 * stayCount, 8000, 12000). Bed allocation
    //                     reaches hard 0 / medium 0 within milliseconds, so the
    //                     whole 10s+ budget is spent on the soft level; a long
    //                     acceptance history pays off there. Measured (10s,
    //                     single thread, 74/150/304-stay instances, 3 seeds)
    //                     against the previous clamp(8 * stayCount, 400, 3200):
    //                     soft +5.7% / +5.1% / +2.6%. 16000 was better still on
    //                     the mid instance but 4.6% worse on the largest, so the
    //                     floor stops at 8000.
    //   ruin_recreate     without the kicks the LA acceptor can lock into a
    //                     local optimum within milliseconds on small instances
    //                     and never escape
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // Assignment move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // --- nearby selection (see solver.cpp) ---
        // Distance between two stays is how far apart their date windows are,
        // so a nearby swap partner occupies roughly the same nights (the swap
        // stays hard-clean) and the LNS related ruin removes one temporal
        // cluster. 0 disables nearby selection entirely.
        //
        // Measured (10s, single thread, 6 seeds) against nearby disabled:
        // soft +0.0% / +1.6% / +4.3% on the 74 / 150 / 304-stay instances.
        // Probability 0.8 and neighbourhood size 40 were both worse.
        double nearby_probability = 0.4;
        int nearby_size = 20;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates stays[*].bed and stores the best score found
    // in plan.score. Every stay ends up with a value — a real bed index or
    // the "no bed" sentinel (plan.noBedValue()), never -1.
    void solve(BedPlan& plan);

    // Solves with a caller-assembled plugin calculator (see
    // ba/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same search applies — construction heuristic,
    // late-acceptance local search, ALNS kicks and the date-window nearby
    // selection. Weights and levels come from the assembly (the components'
    // kDefaultWeight / kDefaultLevel unless the caller changed them). The
    // final plan.score is computed by THIS calculator (its three level
    // accumulators, calculator.level_scores()), and on_new_best reports every new
    // best the same way, so the last reported score equals plan.score. The
    // calculator is left primed on the final solution, so constraint_totals()
    // explains exactly that score. Single-start: the caller-owned calculator
    // cannot be cloned, so parallel_solver_count is ignored.
    void solve(BedPlan& plan, optimaru::DecomposedObjective<BedPlan>& calculator);

private:
    Config config_;
};

}  // namespace ba

#endif  // BA_SOLVER_HPP

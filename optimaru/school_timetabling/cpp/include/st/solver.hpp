#ifndef ST_SOLVER_HPP
#define ST_SOLVER_HPP

#include <utility>

#include "st/domain.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"

namespace st {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<Timetable>. On top of the built-in change/swap moves and
// the engine's adaptive ruin-and-recreate (ALNS) it registers the same-group
// nearby meter adopted on the quality bench, which makes classmates the
// preferred swap partners (see Config::same_group_nearby).
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   history_length 0       clamp(8 * lessonCount, 400, 3200): on the
    //                          quality bench S is insensitive up to 1600 but
    //                          degrades at 3200, while M/L keep improving up to
    //                          3200 (L +108% over the old fixed 400), and 8x
    //                          the lesson count tracks that optimum across the
    //                          presets
    //   parallel_solver_count  the engine's multi-start
    //   ruin_recreate          without the kicks the LA acceptor can lock into
    //                          a local optimum within milliseconds on small
    //                          instances and never escape
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // Destroy-and-repair as an ORDINARY acceptor-judged move, on top of
        // the stagnation kick (engine
        // RuinRecreateSettings::as_move_weight). Probability that a step is a
        // small ruin + greedy repair instead of a sampled change/swap; a
        // rejected one is rolled back like any other move.
        //
        // Negative (the default) = AUTO: 0.2 / slotRoomCount when
        // slotRoomCount is inside the measured pay-off window
        // [900, 1300] cells, and 0.0 (off) outside it. 0 disables, a
        // positive value is used as-is.
        //
        // The window is where the quality bench says it pays. At a weight
        // normalized for cost (0.2 / slotRoomCount, since one of these
        // costs ruinCount * slotRoomCount evaluations): L 1000 cells is
        // +6.1% soft at 10s (6/6 seeds) and +4.1% at 20s (8/10), and 1200
        // cells is +3.3% (3/3). Outside it the same share loses: S 280
        // cells -1.2% (the baseline already sits at the 175 ceiling),
        // M 520 cells -1.3% at 10s with a rare bad tail (one seed -9%),
        // and 1480 cells -5.5% at 10s / -4.8% at 20s — so AUTO switches
        // off rather than extrapolate in either direction. See
        // benchmarks/school-timetabling/RESULTS.md.
        double ruin_recreate_as_move_weight = -1.0;
        int ruin_recreate_as_move_max_ruin = 12;

        // Best-improvement descent over one nearby lesson pair (engine
        // SearchSettings::descent_weight): enumerate every slotRoom for
        // both lessons plus their swap, apply the best improving one, repeat
        // until the pair is locally optimal. Always improving, so it bypasses
        // the acceptor. One descent costs ~2 * slotRoomCount score
        // evaluations, so the weight is a fraction of a percent, not a
        // fraction.
        //
        // Off by default, and weaker here than the as-move above (L: 7/10
        // seeds, +1.8%; a loss on S/M). The neighbourhood it enumerates is
        // the change/swap pair the ordinary move mix already samples, so it
        // mostly pays a thousandfold cost to look where the search was
        // already looking — unlike an employee-rostering domain, where the value range
        // is 40 employees and the same descent is the default. See
        // benchmarks/school-timetabling/RESULTS.md.
        double descent_weight = 0.0;
        int descent_max_sweeps = 4;

        // Assignment move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Same-group nearby selection: lessons of one studentGroup are each
        // other's nearest neighbors, biasing swap partners toward classmates
        // (group-conflict-neutral swaps) and turning the LNS related ruin
        // into a same-group cluster ruin. Mirrors the canonical solver's
        // same_class_nearby; adopted on the quality bench (M/L +10% final
        // soft, S unchanged). A best-of-k smart swap operator like the
        // canonical solver's was also tried here and measured strictly
        // worse (the k-fold evaluation cost starves this domain's very high
        // move throughput), so it is deliberately absent.
        bool same_group_nearby = true;
        double same_group_nearby_probability = 0.5;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates lessons[*].slotRoom and stores the best score
    // found in table.score. Pinned lessons keep their assignment.
    void solve(Timetable& table);

private:
    Config config_;
};

}  // namespace st

#endif  // ST_SOLVER_HPP

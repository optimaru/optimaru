#ifndef ST_SOLVER_HPP
#define ST_SOLVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "st/domain.hpp"
#include "optimaru/api/score/director/score_director.hpp"

namespace st
{

// Thin domain-side wrapper around the generic engine in include/optimaru/ — the C++
// analog of a quickstart building a SolverFactory from a SolverConfig. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<Timetable>. On top of the built-in change/swap moves and
// the engine's adaptive ruin-and-recreate (ALNS) it registers the same-group
// nearby meter adopted on the quality bench, which makes classmates the
// preferred swap partners (see Config::sameGroupNearby).
class Solver
{
public:
    struct Config
    {
        // Wall-clock budget, mirroring termination.spent-limit (30s in the
        // quickstart's application.properties).
        std::chrono::milliseconds spentLimit{std::chrono::seconds(30)};

        // Stop early once bestScore >= bestScoreLimit. Default is above every
        // achievable score, so it never fires (termination is by spentLimit).
        Score bestScoreLimit{std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<double>::infinity()};
        // Optional early exits; 0 disables each criterion.
        std::chrono::milliseconds unimprovedSpentLimit{0};
        std::int64_t stepCountLimit = 0;
        std::int64_t unimprovedStepCountLimit = 0;

        // Late-acceptance history length. 0 (the default) = auto: clamp(8 *
        // lessonCount, 400, 3200) — on the quality bench S is insensitive up
        // to 1600 but degrades at 3200, while M/L keep improving up to 3200
        // (L +108% over the old fixed 400), and 8x the lesson count tracks
        // that optimum across the presets.
        std::size_t lateAcceptanceSize = 0;
        std::uint64_t randomSeed = 0;

        // Independent parallel starts (the engine's multi-start): each worker
        // receives randomSeed + i and the full wall-clock budget; the best
        // solution wins.
        int parallelSolverCount = 1;

        // FULL_ASSERT re-verifies the incremental score against a full
        // recompute after every move (debugging aid; orders of magnitude
        // slower).
        optimaru::EnvironmentMode environmentMode
            = optimaru::EnvironmentMode::NON_ASSERT;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS). Without this the LA acceptor can lock into a local
        // optimum within milliseconds on small instances and never escape.
        bool ruinRecreate = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;

        // Destroy-and-repair as an ORDINARY acceptor-judged move, on top of
        // the stagnation kick above (engine
        // RuinRecreateConfig::asMoveWeight). Probability that a step is a
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
        double ruinRecreateAsMoveWeight = -1.0;
        int ruinRecreateAsMoveMaxRuin = 12;

        // Best-improvement descent over one nearby lesson pair (engine
        // LocalSearchConfig::descentWeight): enumerate every slotRoom for
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
        double descentWeight = 0.0;
        int descentMaxSweeps = 4;

        // BASIC move mix. Weights are relative; 0 disables a move.
        double changeMoveWeight = 0.75;
        double swapMoveWeight = 0.25;

        // Same-group nearby selection: lessons of one studentGroup are each
        // other's nearest neighbors, biasing swap partners toward classmates
        // (group-conflict-neutral swaps) and turning the LNS related ruin
        // into a same-group cluster ruin. Mirrors the canonical solver's
        // sameClassNearby; adopted on the quality bench (M/L +10% final
        // soft, S unchanged). A best-of-k smart swap operator like the
        // canonical solver's was also tried here and measured strictly
        // worse (the k-fold evaluation cost starves this domain's very high
        // move throughput), so it is deliberately absent.
        bool sameGroupNearby = true;
        double sameGroupNearbyProbability = 0.5;

        std::function<void(std::int64_t, const Score &)> onNewBest;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates lessons[*].slotRoom and stores the best score
    // found in table.score. Pinned lessons keep their assignment.
    void solve(Timetable &table);

private:
    Config config_;
};

}  // namespace st

#endif  // ST_SOLVER_HPP

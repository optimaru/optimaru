#ifndef QAP_SOLVER_HPP
#define QAP_SOLVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>

#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/config/localsearch/decider/acceptor/local_search_acceptor_config.hpp"
#include "qap/domain.hpp"

namespace qap
{

// Thin domain-side wrapper around the generic engine in include/optimaru/: it
// starts from a complete permutation (the identity, or a warm-start seed that
// must be a bijection), wires up the incremental score calculator and
// delegates the search to optimaru::Solver<QuadraticAssignmentProblem> —
// SWAP moves only (two facilities exchange their locations), so every state
// the search visits is a permutation, judged by the configured acceptor
// (late acceptance by default).
//
// Deliberately NOT exposed, because each assigns values one entity at a time
// and would break the permutation: change moves (the engine also falls back
// to a change move when the swap weight is 0), ruin-and-recreate, descent,
// value recommenders, ejection chains and iterated local search. The final
// state is checked to be a bijection (std::logic_error otherwise).
class Solver
{
public:
    struct Config
    {
        // Wall-clock budget. 0 stops before the first step: the solve then
        // returns its start (the identity, or the seed) unchanged.
        std::chrono::milliseconds spentLimit{std::chrono::seconds(30)};

        // Stop early once bestScore >= bestScoreLimit. Default is above every
        // achievable score, so it never fires.
        Score bestScoreLimit{std::numeric_limits<std::int64_t>::max(), std::numeric_limits<double>::infinity()};
        // Optional early exits; 0 disables each criterion (so
        // stepCountLimit = 0 means "no step limit", not "no search").
        std::chrono::milliseconds unimprovedSpentLimit{0};
        std::int64_t stepCountLimit = 0;
        std::int64_t unimprovedStepCountLimit = 0;

        // The local-search acceptor. Late acceptance by default; the history
        // length 0 (the default) = auto: clamp(8 * facility count, 400, 3200).
        optimaru::AcceptorType acceptorType = optimaru::AcceptorType::LATE_ACCEPTANCE;
        std::size_t lateAcceptanceSize = 0;
        int entityTabuSize = 7;  // ENTITY_TABU
        // SIMULATED_ANNEALING's starting temperature (hard, soft). (0, 0), the
        // default, = auto: hard 0, soft = simulatedAnnealingTemperatureFraction
        // * |the start's soft score| (at least 1), the semiconductor-scheduling
        // rule. The engine's own default (0, 0) would make SA reject every
        // worsening swap, i.e. hill climbing. It cools to 0 over the time
        // limit, or over stepCountLimit when that is further along.
        Score simulatedAnnealingStartingTemperature{0, 0.0};
        double simulatedAnnealingTemperatureFraction = 0.01;
        std::uint64_t randomSeed = 0;

        // Independent parallel starts (the engine's multi-start): worker i
        // searches its own copy of the start with randomSeed + i and the full
        // budget; the best score wins. Ignored by the constraint-assembly
        // overload.
        int parallelSolverCount = 1;

        // FULL_ASSERT re-verifies the incremental score against a full
        // recompute after every move (debugging aid; much slower).
        optimaru::EnvironmentMode environmentMode = optimaru::EnvironmentMode::NON_ASSERT;

        std::function<void(std::int64_t, const Score &)> onNewBest;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: prepares (validating the facts), starts from the
    // identity or — with problem.warmStart — from the current assignment (a
    // full bijection, else std::invalid_argument), searches with swaps,
    // writes the best permutation and problem.score (a full recompute), and
    // resets problem.warmStart to false.
    void solve(QuadraticAssignmentProblem &problem);

    // Solves with a caller-assembled plugin calculator (see
    // qap/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same start and search apply. The final problem.score
    // is computed by THIS calculator and the calculator is left primed on the
    // final solution, so constraintTotals() explains exactly that score.
    // Single-start: the caller-owned calculator cannot be cloned, so
    // parallelSolverCount is ignored.
    void solve(QuadraticAssignmentProblem &problem,
               optimaru::DecomposedScoreCalculator<QuadraticAssignmentProblem> &calculator);

private:
    Config config_;
};

}  // namespace qap

#endif  // QAP_SOLVER_HPP

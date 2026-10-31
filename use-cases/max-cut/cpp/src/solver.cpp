#include "mc/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "mc/incremental_calculator.hpp"
#include "mc/score_calculator.hpp"
#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"

namespace mc
{

namespace
{

void requireRewards(const Solver::Config &config)
{
    if (config.ruinRecreateRewards.size() != 4)
        throw std::invalid_argument("ruinRecreateRewards must contain exactly 4 values");
}

// The engine configuration of one search, shared by both solve() overloads
// and every parallel start: late-acceptance local search over the built-in
// BASIC moves, plus the ALNS kicks. The engine's construction heuristic finds
// nothing left to place: the start partition assigned every vertex.
optimaru::SolverConfig engineConfigOf(const Solver::Config &config, const MaxCutProblem &problem)
{
    optimaru::SolverConfig engineConfig;
    engineConfig.randomSeed = config.randomSeed;
    engineConfig.environmentMode = config.environmentMode;
    engineConfig.onNewBest = config.onNewBest;
    engineConfig.termination.spentLimit = config.spentLimit;
    engineConfig.termination.bestScoreLimit = config.bestScoreLimit;
    engineConfig.termination.unimprovedSpentLimit = config.unimprovedSpentLimit;
    engineConfig.termination.stepCountLimit = config.stepCountLimit;
    engineConfig.termination.unimprovedStepCountLimit = config.unimprovedStepCountLimit;
    engineConfig.localSearch.acceptor.lateAcceptanceSize
        = config.lateAcceptanceSize != 0
              ? config.lateAcceptanceSize
              : std::clamp<std::size_t>(8 * problem.vertexIds.size(), 400, 3200);
    engineConfig.localSearch.moveSelector.changeMoveWeight = config.changeMoveWeight;
    engineConfig.localSearch.moveSelector.swapMoveWeight = config.swapMoveWeight;
    engineConfig.localSearch.descentWeight = config.descentWeight;

    auto &lns = engineConfig.localSearch.ruinRecreate;
    lns.enabled = config.ruinRecreate;
    lns.unimprovedStepTrigger = config.ruinRecreateUnimprovedStepTrigger;
    lns.minRuinFraction = config.ruinRecreateMinRuinFraction;
    lns.maxRuinFraction = config.ruinRecreateMaxRuinFraction;
    lns.maxRuinCount = config.ruinRecreateMaxRuinCount;
    lns.operatorDecay = config.ruinRecreateOperatorDecay;
    lns.rewards = config.ruinRecreateRewards;
    lns.polishSteps = config.ruinRecreatePolishSteps;
    lns.asMoveWeight = config.ruinRecreateAsMoveWeight;
    lns.asMoveMaxRuin = config.ruinRecreateAsMoveMaxRuin;

    if (problem.exactSize())
    {
        // Swaps only: every other move (a flip, a ruin's unassign-and-recreate,
        // a descent's change) could change a side size.
        engineConfig.localSearch.moveSelector.changeMoveWeight = 0.0;
        engineConfig.localSearch.moveSelector.swapMoveWeight = 1.0;
        engineConfig.localSearch.descentWeight = 0.0;
        lns.enabled = false;
        lns.asMoveWeight = 0.0;
    }
    return engineConfig;
}

// Validates, builds the start partition and consumes the seed.
void construct(MaxCutProblem &problem)
{
    ScoreCalculator::prepare(problem);  // validates the facts and the partition
    if (!problem.warmStart)
        std::fill(problem.side.begin(), problem.side.end(), kUnassigned);
    for (int v = 0; v < problem.vertexCount(); ++v)
    {
        if (problem.side[v] == kUnassigned)
            problem.side[v] = problem.isPinned(v) ? problem.fixedSide[v] : v % kSideCount;
    }
    problem.warmStart = false;  // a seed is used once
}

// Whether the search has anything to do: an empty graph has no move, and in
// exact-size mode a swap needs two free vertices (the engine's fallback
// for fewer entities is a change move, which would break the size).
bool searchable(const MaxCutProblem &problem)
{
    if (problem.vertexCount() == 0)
        return false;
    return !problem.exactSize() || problem.movableCount() >= 2;
}

// Independent parallel starts: worker i searches its own copy of the start
// partition with randomSeed + i and the full budget (only worker 0 reports
// onNewBest); the best score wins, the lowest index on a tie. This is the
// search of the engine's solveMultiStart, run on worker threads that catch
// their own exceptions: a throwing onNewBest, a FULL_ASSERT
// ScoreCorruptionError or a bad_alloc in one worker stops the others early
// and is rethrown here once every worker has joined, instead of terminating
// the process.
void searchInParallel(const Solver::Config &config, MaxCutProblem &problem, int starts)
{
    std::vector<MaxCutProblem> copies(starts, problem);
    std::vector<optimaru::HardSoftScore> scores(starts);
    std::vector<std::exception_ptr> failures(starts);
    // Every engine exists before any worker starts, so a failing worker can
    // stop all of them (terminateEarly() is thread-safe and sticky).
    std::vector<std::unique_ptr<optimaru::Solver<MaxCutProblem>>> engines;
    engines.reserve(starts);
    for (int i = 0; i < starts; ++i)
    {
        optimaru::SolverConfig engineConfig = engineConfigOf(config, problem);
        engineConfig.randomSeed += static_cast<std::uint64_t>(i);
        if (i != 0)
            engineConfig.onNewBest = nullptr;
        engines.push_back(std::make_unique<optimaru::Solver<MaxCutProblem>>(std::move(engineConfig)));
    }
    std::vector<std::thread> workers;
    workers.reserve(starts);
    for (int i = 0; i < starts; ++i)
    {
        workers.emplace_back(
            [i, &copies, &scores, &failures, &engines]
            {
                try
                {
                    MaxCutIncrementalScoreCalculator calculator;
                    scores[i] = engines[i]->solve(copies[i], calculator);
                }
                catch (...)
                {
                    failures[i] = std::current_exception();
                    for (const auto &engine : engines)
                        engine->terminateEarly();
                }
            });
    }
    for (std::thread &worker : workers)
        worker.join();
    for (const std::exception_ptr &failure : failures)
    {
        if (failure)
            std::rethrow_exception(failure);
    }
    int best = 0;
    for (int i = 1; i < starts; ++i)
    {
        if (scores[i] > scores[best])
            best = i;
    }
    problem = std::move(copies[best]);
}

}  // namespace

void Solver::solve(MaxCutProblem &problem)
{
    requireRewards(config_);
    construct(problem);
    if (searchable(problem))
    {
        const int starts = std::max(1, config_.parallelSolverCount);
        if (starts == 1)
        {
            MaxCutIncrementalScoreCalculator calculator;
            optimaru::Solver<MaxCutProblem> engine(engineConfigOf(config_, problem));
            engine.solve(problem, calculator);
        }
        else
        {
            searchInParallel(config_, problem, starts);
        }
    }
    // The authoritative final score: a full recompute on the best solution.
    ScoreCalculator::calculateScore(problem);
}

void Solver::solve(MaxCutProblem &problem, optimaru::DecomposedScoreCalculator<MaxCutProblem> &calculator)
{
    requireRewards(config_);
    construct(problem);
    if (searchable(problem))
    {
        // Single-start: the caller-owned calculator cannot be cloned.
        optimaru::Solver<MaxCutProblem> engine(engineConfigOf(config_, problem));
        engine.solve(problem, calculator);
    }
    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraintTotals() explains exactly this score.
    calculator.resetWorkingSolution(problem);
    problem.score = calculator.calculateScore();
    problem.scored = true;
}

}  // namespace mc

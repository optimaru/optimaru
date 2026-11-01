#include "alb/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "alb/construction.hpp"
#include "alb/incremental_calculator.hpp"
#include "alb/score_calculator.hpp"
#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"
#include "optimaru/impl/solver/worker_threads.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace alb
{

namespace
{

// The stations a task may move to without breaking a precedence arc against
// the CURRENT stations of its neighbours or leaving its own domain:
// [latest predecessor station, earliest successor station] intersected with
// [min_station, max_station], below max_stations and inside [0, V).
// Unassigned neighbours do not narrow it. Empty when lo > hi.
struct Window
{
    int lo;
    int hi;
};

Window windowOf(const AssemblyLineBalancingProblem &problem, int task)
{
    const Task &t = problem.tasks[task];
    int lo = std::max(0, t.minStation);
    int hi = problem.stationCount - 1;
    if (t.maxStation)
        hi = std::min(hi, *t.maxStation);
    if (problem.maxStations)
        hi = std::min(hi, *problem.maxStations - 1);
    for (int arc : problem.arcsIn[task])
    {
        const int before = problem.tasks[problem.precedences[arc].before].station;
        if (before >= 0)
            lo = std::max(lo, before);
    }
    for (int arc : problem.arcsOut[task])
    {
        const int after = problem.tasks[problem.precedences[arc].after].station;
        if (after >= 0)
            hi = std::min(hi, after);
    }
    return {lo, hi};
}

// ---------------------------------------------------------------------------
// CloseStationMove — the domain's missing macro move
// ---------------------------------------------------------------------------
// usedStationsCost (the SALBP-1 objective) is a step function of "does this
// station hold any task": a station only stops costing once its LAST task
// leaves, so every single-task change on the way there is no better, and the
// hard constraints usually make it worse. This operator does the whole step
// in one move: pick a used station (a 2-tournament biased toward the lighter
// one) and empty it, moving its tasks in topological order, each to the
// nearest station inside its precedence / domain window that is already in
// use and still has room for it; else the nearest one in the window with room;
// else the less loaded neighbour of the emptied station. The acceptor then
// judges the complete trade — one score evaluation for the whole batch.
//
// Cost is O(tasks + stations) per move (a census plus a short outward scan per
// moved task); the adaptive roulette wheel decides how often it is worth it.
class CloseStationMove final : public optimaru::MoveOperator<AssemblyLineBalancingProblem>
{
public:
    explicit CloseStationMove(std::shared_ptr<const std::vector<int>> rank)
        : rank_(std::move(rank))
    {
    }

    const char *name() const override { return "closeStation"; }

    bool apply(optimaru::OperatorContext<AssemblyLineBalancingProblem> &ctx,
               std::mt19937_64 &rng,
               std::vector<int> &entities) override
    {
        AssemblyLineBalancingProblem &problem = ctx.director.workingSolution();
        const int stationCount = problem.stationCount;
        const int taskCount = problem.taskCount();

        load_.assign(stationCount, 0);
        count_.assign(stationCount, 0);
        for (const Task &task : problem.tasks)
        {
            if (task.station >= 0)
            {
                load_[task.station] += task.duration;
                ++count_[task.station];
            }
        }
        used_.clear();
        for (int s = 0; s < stationCount; ++s)
        {
            if (count_[s] > 0)
                used_.push_back(s);
        }
        if (used_.size() < 2)
            return false;  // nowhere to move the tasks to

        int victim = used_[rng() % used_.size()];
        const int rival = used_[rng() % used_.size()];
        if (load_[rival] < load_[victim])
            victim = rival;

        members_.clear();
        for (int t = 0; t < taskCount; ++t)
        {
            if (problem.tasks[t].station == victim)
                members_.push_back(t);
        }
        const std::vector<int> &rank = *rank_;
        std::sort(members_.begin(), members_.end(),
                  [&rank](int a, int b) { return rank[a] < rank[b]; });

        // The victim is being emptied: it is never a target.
        load_[victim] = 0;
        count_[victim] = 0;
        lastVictim_ = victim;

        ctx.director.beginVariableChanges();
        for (int t : members_)
        {
            const int target = pickTarget(problem, t, victim);
            ctx.director.changeVariable(t, target);
            load_[target] += problem.tasks[t].duration;
            ++count_[target];
            entities.push_back(t);
        }
        ctx.director.endVariableChanges();
        return true;
    }

    void undoLast(optimaru::OperatorContext<AssemblyLineBalancingProblem> &ctx) override
    {
        ctx.director.beginVariableChanges();
        for (int t : members_)
            ctx.director.changeVariable(t, lastVictim_);
        ctx.director.endVariableChanges();
    }

private:
    int pickTarget(const AssemblyLineBalancingProblem &problem, int task, int victim) const
    {
        const int stationCount = problem.stationCount;
        const std::int64_t duration = problem.tasks[task].duration;
        const Window window = windowOf(problem, task);
        auto fits = [&](int s) { return load_[s] + duration <= problem.cycleTime; };
        int roomy = -1;  // nearest station in the window with room (maybe unused)
        for (int k = 1; k < stationCount; ++k)
        {
            const int below = victim - k;
            const int above = victim + k;
            const bool belowIn = below >= window.lo && below <= window.hi && below >= 0;
            const bool aboveIn = above >= window.lo && above <= window.hi && above < stationCount;
            if (belowIn && fits(below))
            {
                if (count_[below] > 0)
                    return below;
                if (roomy < 0)
                    roomy = below;
            }
            if (aboveIn && fits(above))
            {
                if (count_[above] > 0)
                    return above;
                if (roomy < 0)
                    roomy = above;
            }
            if (below < window.lo && above > window.hi)
                break;
        }
        if (roomy >= 0)
            return roomy;
        // Nothing fits inside the window: the less loaded neighbour.
        const int below = victim - 1;
        const int above = victim + 1;
        if (below < 0)
            return above < stationCount ? above : victim;
        if (above >= stationCount)
            return below;
        return load_[above] < load_[below] ? above : below;
    }

    std::shared_ptr<const std::vector<int>> rank_;  // topological rank per task

    // Scratch, reused across moves (no per-move allocation).
    std::vector<std::int64_t> load_;
    std::vector<int> count_;
    std::vector<int> used_;
    std::vector<int> members_;
    int lastVictim_ = -1;
};

// Prepares the problem, runs the domain construction and freezes the station
// value range on the constructed stations. Returns the topological rank of
// every task (CloseStationMove moves a station's tasks in that order).
std::shared_ptr<const std::vector<int>> construct(AssemblyLineBalancingProblem &problem)
{
    ScoreCalculator::prepare(problem);  // validates the facts and the stations
    // Always computed, so a cyclic graph is refused even for a complete seed.
    const std::vector<int> order = topologicalOrder(problem);
    if (!problem.warmStart)
    {
        for (Task &task : problem.tasks)
            task.station = -1;
    }
    constructStations(problem, order);
    ScoreCalculator::prepare(problem);  // V = max(n, 1 + max station), frozen
    auto rank = std::make_shared<std::vector<int>>(problem.tasks.size(), 0);
    for (std::size_t position = 0; position < order.size(); ++position)
        (*rank)[order[position]] = static_cast<int>(position);
    return rank;
}

// Shared engine wiring for both solve() overloads: late-acceptance local
// search (+ ALNS kicks), plus the domain's macro move and value recommender,
// driven by whichever calculator the caller brought (the monolithic
// incremental one, or a plugin assembly). The engine's construction heuristic
// finds nothing left to place: the domain construction assigned every task.
void runEngine(const Solver::Config &config,
               AssemblyLineBalancingProblem &problem,
               const std::shared_ptr<const std::vector<int>> &rank,
               optimaru::IncrementalScoreCalculator<AssemblyLineBalancingProblem> &calculator)
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
              : std::clamp<std::size_t>(8 * problem.tasks.size(), 400, 3200);
    engineConfig.localSearch.moveSelector.changeMoveWeight = config.changeMoveWeight;
    engineConfig.localSearch.moveSelector.swapMoveWeight = config.swapMoveWeight;
    engineConfig.localSearch.customMoveProbability = config.customMoveProbability;
    engineConfig.localSearch.descentWeight = config.descentWeight;

    // Guided change-move targets: the task's current precedence / domain
    // window (-1 falls back to the engine's uniform draw). The engine solves
    // in place, so the problem captured here IS the working solution and the
    // window reads the live stations.
    if (config.valueRecommenderProbability > 0.0)
    {
        const AssemblyLineBalancingProblem &live = problem;
        const double p = config.valueRecommenderProbability;
        engineConfig.localSearch.valueRecommender
            = [&live, p](std::mt19937_64 &rng, int task) -> int
        {
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            if (unit(rng) >= p)
                return -1;
            const Task &t = live.tasks[task];
            if (t.fixedStation && *t.fixedStation >= 0 && *t.fixedStation < live.stationCount)
                return *t.fixedStation;
            const Window window = windowOf(live, task);
            if (window.lo > window.hi)
                return -1;
            return window.lo
                   + static_cast<int>(rng()
                                      % static_cast<std::uint64_t>(window.hi - window.lo + 1));
        };
    }

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

    optimaru::Solver<AssemblyLineBalancingProblem> engine(std::move(engineConfig));
    if (config.stopFlag)
        engine.adoptTerminateEarlyFlag(config.stopFlag);
    if (config.closeStationMoves && problem.stationCount >= 2)
        engine.addMoveOperator(std::make_unique<CloseStationMove>(rank));
    engine.solve(problem, calculator);
}

void requireRewards(const Solver::Config &config)
{
    if (config.ruinRecreateRewards.size() != 4)
        throw std::invalid_argument("ruinRecreateRewards must contain exactly 4 values");
}

// One start on an already constructed problem, scored by the monolithic
// calculators.
void searchConstructed(const Solver::Config &config,
                       AssemblyLineBalancingProblem &problem,
                       const std::shared_ptr<const std::vector<int>> &rank)
{
    AssemblyLineBalancingIncrementalScoreCalculator calculator;
    runEngine(config, problem, rank, calculator);
    // The authoritative final score: a full recompute on the best solution.
    problem.score = ScoreCalculator::calculateScore(problem);
}

}  // namespace

void Solver::solve(AssemblyLineBalancingProblem &problem)
{
    requireRewards(config_);
    const std::shared_ptr<const std::vector<int>> rank = construct(problem);
    problem.warmStart = false;  // a seed is used once
    if (problem.tasks.empty())
    {
        ScoreCalculator::calculateScore(problem);
        return;
    }

    // Independent parallel starts: worker i searches its own copy of the
    // constructed problem with seed + i. The engine's own multi-start cannot
    // be used, because a registered custom operator like CloseStationMove is
    // per-Solver and stateful, so it would silently vanish in the worker
    // threads. The best score wins (the lowest worker index on ties).
    const int workerCount = std::max(1, config_.parallelSolverCount);
    if (workerCount > 1)
    {
        std::vector<AssemblyLineBalancingProblem> candidates(workerCount, problem);
        std::vector<std::exception_ptr> failures(workerCount);
        // Raised by the first worker that fails: every worker's engine checks
        // it once a step, so the others stop instead of using up their budget.
        auto stop = std::make_shared<std::atomic<bool>>(false);
        // A worker that cannot be started (std::system_error) stops and joins
        // the ones already running before the error is rethrown here.
        optimaru::runWorkerThreads(
            workerCount, *stop,
            [this, &candidates, &failures, &stop, &rank](int i)
            {
                try
                {
                    Config workerConfig = config_;
                    workerConfig.parallelSolverCount = 1;
                    workerConfig.stopFlag = stop;
                    workerConfig.randomSeed += static_cast<std::uint64_t>(i);
                    if (i != 0)
                        workerConfig.onNewBest = nullptr;
                    searchConstructed(workerConfig, candidates[i], rank);
                }
                catch (...)
                {
                    failures[i] = std::current_exception();
                    stop->store(true);
                }
            });
        for (const std::exception_ptr &failure : failures)
        {
            if (failure)
                std::rethrow_exception(failure);
        }
        int best = 0;
        for (int i = 1; i < workerCount; ++i)
        {
            if (candidates[i].score > candidates[best].score)
                best = i;
        }
        problem = std::move(candidates[best]);
        return;
    }

    searchConstructed(config_, problem, rank);
}

void Solver::solve(AssemblyLineBalancingProblem &problem,
                   optimaru::DecomposedScoreCalculator<AssemblyLineBalancingProblem> &calculator)
{
    requireRewards(config_);
    const std::shared_ptr<const std::vector<int>> rank = construct(problem);
    problem.warmStart = false;  // a seed is used once
    if (!problem.tasks.empty())
        runEngine(config_, problem, rank, calculator);

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraintTotals() explains exactly this score.
    calculator.resetWorkingSolution(problem);
    problem.score = calculator.calculateScore();
}

}  // namespace alb

#include "bp/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bp/construction.hpp"
#include "bp/incremental_calculator.hpp"
#include "bp/score_calculator.hpp"
#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace bp
{

namespace
{

// ---------------------------------------------------------------------------
// EmptyBinMove — the domain's missing macro move (0.1.5 `_try_empty_bin`)
// ---------------------------------------------------------------------------
// usedBinsCost is a step function of "does this bin hold anything": a bin
// only stops costing once its LAST item leaves, so every single-item change
// on the way there is no better and usually breaks the capacity of the bin it
// lands in. This operator does the whole step in one move: take one of the
// `candidates` lightest non-empty bins (uniformly; random among equal loads)
// and move its items, heaviest first (random among equal weights), each to
// the best-fitting OTHER non-empty bin that still has room. It gives up (no
// move) when some item fits nowhere. Like 0.1.5 it checks the capacity only:
// colours and conflicts are judged by the score, through the acceptor, like
// everything else.
//
// Cost is O(n + used bins * moved items) per move (a census plus a best-fit
// scan per moved item); the adaptive roulette wheel decides how often it is
// worth that.
class EmptyBinMove final : public optimaru::MoveOperator<BinPackingProblem>
{
public:
    explicit EmptyBinMove(int candidates) : candidates_(std::max(1, candidates)) {}

    const char *name() const override { return "emptyBin"; }

    bool apply(optimaru::OperatorContext<BinPackingProblem> &ctx,
               std::mt19937_64 &rng,
               std::vector<int> &entities) override
    {
        BinPackingProblem &problem = ctx.director.workingSolution();
        const int n = problem.itemCount();
        const std::int64_t capacity = problem.binCapacity;

        load_.assign(static_cast<std::size_t>(n), 0);
        count_.assign(static_cast<std::size_t>(n), 0);
        for (const Item &item : problem.items)
        {
            if (item.bin >= 0)
            {
                load_[item.bin] += item.weight;
                ++count_[item.bin];
            }
        }
        used_.clear();
        for (int b = 0; b < n; ++b)
        {
            if (count_[b] > 0)
                used_.push_back({load_[b], rng(), b});
        }
        if (used_.size() < 2)
            return false;  // nowhere to move the items to

        // The k lightest bins, random among equal loads.
        const std::size_t k
            = std::min<std::size_t>(static_cast<std::size_t>(candidates_), used_.size());
        std::partial_sort(used_.begin(),
                          used_.begin() + static_cast<std::ptrdiff_t>(k),
                          used_.end());
        const int source = used_[rng() % k].bin;

        members_.clear();
        for (int i = 0; i < n; ++i)
        {
            if (problem.items[i].bin == source)
                members_.push_back(i);
        }
        std::shuffle(members_.begin(), members_.end(), rng);
        std::stable_sort(members_.begin(),
                         members_.end(),
                         [&problem](int a, int b)
                         { return problem.items[a].weight > problem.items[b].weight; });

        // Plan every target first, so a failure leaves the solution as it is.
        targets_.clear();
        for (int item : members_)
        {
            const std::int64_t weight = problem.items[item].weight;
            int target = -1;
            std::int64_t bestRoom = 0;
            for (const Candidate &candidate : used_)
            {
                const int bin = candidate.bin;
                if (bin == source || load_[bin] + weight > capacity)
                    continue;
                const std::int64_t room = capacity - load_[bin] - weight;
                if (target < 0 || room < bestRoom || (room == bestRoom && bin < target))
                {
                    target = bin;
                    bestRoom = room;
                }
            }
            if (target < 0)
                return false;
            load_[target] += weight;
            targets_.push_back(target);
        }

        lastSource_ = source;
        ctx.director.beginVariableChanges();
        for (std::size_t m = 0; m < members_.size(); ++m)
        {
            ctx.director.changeVariable(members_[m], targets_[m]);
            entities.push_back(members_[m]);
        }
        ctx.director.endVariableChanges();
        return true;
    }

    void undoLast(optimaru::OperatorContext<BinPackingProblem> &ctx) override
    {
        ctx.director.beginVariableChanges();
        for (int item : members_)
            ctx.director.changeVariable(item, lastSource_);
        ctx.director.endVariableChanges();
    }

private:
    struct Candidate
    {
        std::int64_t load;
        std::uint64_t tieBreak;
        int bin;
        bool operator<(const Candidate &other) const
        {
            if (load != other.load)
                return load < other.load;
            return tieBreak < other.tieBreak;
        }
    };

    int candidates_;

    // Scratch, reused across moves (no per-move allocation).
    std::vector<std::int64_t> load_;
    std::vector<int> count_;
    std::vector<Candidate> used_;
    std::vector<int> members_;
    std::vector<int> targets_;
    int lastSource_ = -1;
};

void requireRewards(const Solver::Config &config)
{
    if (config.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }
}

// The 0.1.5 solver checks, in its order and words.
void validate(const BinPackingProblem &problem)
{
    if (problem.binCapacity <= 0)
        throw std::invalid_argument("bin_capacity must be positive");
    std::unordered_set<std::string> ids;
    for (const Item &item : problem.items)
    {
        if (!ids.insert(item.id).second)
            throw std::invalid_argument("item ids must be unique");
    }
    for (const Item &item : problem.items)
    {
        if (item.weight <= 0)
            throw std::invalid_argument("item weights must be positive");
    }
}

// Validates, drops the input bins of a plain solve and builds the derived
// lookups. Leaves the problem ready for constructBins.
void prepareSolve(BinPackingProblem &problem)
{
    validate(problem);
    if (!problem.warmStart)
    {
        for (Item &item : problem.items)
            item.bin = -1;  // a plain solve ignores the input bins
    }
    ScoreCalculator::prepare(problem);
}

// Shared engine wiring for both solve() overloads: late-acceptance local
// search (+ ALNS kicks), plus the domain's macro move and value recommender,
// driven by whichever calculator the caller brought (the monolithic
// incremental one, or a plugin assembly). The engine's construction heuristic
// finds nothing left to place: constructBins assigned every item.
void runEngine(const Solver::Config &config,
               BinPackingProblem &problem,
               optimaru::IncrementalScoreCalculator<BinPackingProblem> &calculator)
{
    optimaru::SolverConfig engineConfig;
    engineConfig.randomSeed = config.randomSeed;
    engineConfig.environmentMode = config.environmentMode;
    engineConfig.onNewBest = config.onNewBest;
    engineConfig.termination.spentLimit = config.spentLimit;
    engineConfig.termination.bestScoreLimit = config.bestScoreLimit;
    engineConfig.termination.unimprovedSpentLimit = config.unimprovedSpentLimit;
    engineConfig.termination.stepCountLimit = config.stepCountLimit;
    engineConfig.termination.unimprovedStepCountLimit
        = config.unimprovedStepCountLimit;
    // lateAcceptanceSize 0 = auto-scale with the instance (see Config).
    engineConfig.localSearch.acceptor.lateAcceptanceSize
        = config.lateAcceptanceSize != 0
              ? config.lateAcceptanceSize
              : std::clamp<std::size_t>(8 * problem.items.size(), 400, 3200);
    engineConfig.localSearch.moveSelector.changeMoveWeight
        = config.changeMoveWeight;
    engineConfig.localSearch.moveSelector.swapMoveWeight = config.swapMoveWeight;
    engineConfig.localSearch.customMoveProbability
        = config.customMoveProbability;
    engineConfig.localSearch.descentWeight = config.descentWeight;

    // Guided change-move targets: the bin of a uniformly drawn other item (a
    // bin in use; -1 falls back to the engine's uniform draw over all n
    // bins). The engine solves in place, so the problem captured here IS the
    // working solution and the recommender reads the live bins.
    if (config.valueRecommenderProbability > 0.0 && problem.itemCount() >= 2)
    {
        const BinPackingProblem &live = problem;
        const double p = config.valueRecommenderProbability;
        engineConfig.localSearch.valueRecommender
            = [&live, p](std::mt19937_64 &rng, int item) -> int
        {
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            if (unit(rng) >= p)
                return -1;
            const auto n = static_cast<std::uint64_t>(live.items.size());
            int other = static_cast<int>(rng() % (n - 1));
            if (other >= item)
                ++other;  // never the item itself
            return live.items[other].bin;  // may be -1 inside a ruin: uniform
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

    optimaru::Solver<BinPackingProblem> engine(std::move(engineConfig));
    if (config.emptyBinMoves && problem.itemCount() >= 2)
        engine.addMoveOperator(std::make_unique<EmptyBinMove>(config.emptyBinCandidates));
    engine.solve(problem, calculator);
}

// After the search: relabel the bins 0..k-1 when anything moved (a result
// equal to its start keeps its labels, so a zero-budget seed comes back as
// it was).
void compactIfMoved(BinPackingProblem &problem, const std::vector<int> &start)
{
    if (problem.assignment() != start)
        compactBins(problem);
}

// One start on a prepared problem, scored by the monolithic calculators.
void searchOneStart(const Solver::Config &config,
                    BinPackingProblem &problem,
                    const std::vector<int> &order)
{
    constructBins(problem, order);
    const std::vector<int> start = problem.assignment();
    BinPackingIncrementalScoreCalculator calculator;
    runEngine(config, problem, calculator);
    compactIfMoved(problem, start);
    // The authoritative final score: a full recompute on the best solution.
    problem.score = ScoreCalculator::calculateScore(problem);
}

}  // namespace

void Solver::solve(BinPackingProblem &problem)
{
    requireRewards(config_);
    prepareSolve(problem);
    problem.warmStart = false;  // a seed is used once
    if (problem.items.empty())
    {
        ScoreCalculator::calculateScore(problem);
        return;
    }

    // Independent parallel starts: worker i constructs and searches its own
    // copy with seed + i. The engine's own multi-start cannot be used,
    // because a registered custom operator like EmptyBinMove is per-Solver
    // and stateful, so it would silently vanish in the worker threads. The
    // best score wins (the lowest worker index on ties).
    const int workerCount = std::max(1, config_.parallelSolverCount);
    const std::vector<int> descending = descendingOrder(problem);
    const bool seeded = std::any_of(problem.items.begin(),
                                    problem.items.end(),
                                    [](const Item &item) { return item.bin >= 0; });
    if (workerCount == 1)
    {
        searchOneStart(config_, problem, descending);
        return;
    }

    std::vector<BinPackingProblem> candidates(workerCount, problem);
    std::vector<std::exception_ptr> failures(workerCount);
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (int i = 0; i < workerCount; ++i)
    {
        workers.emplace_back(
            [this, i, seeded, &descending, &candidates, &failures]
            {
                try
                {
                    Config workerConfig = config_;
                    workerConfig.parallelSolverCount = 1;
                    workerConfig.randomSeed += static_cast<std::uint64_t>(i);
                    if (i != 0)
                        workerConfig.onNewBest = nullptr;
                    std::mt19937_64 rng(workerConfig.randomSeed);
                    const std::vector<int> order
                        = (i == 0 || seeded) ? descending
                                             : tieShuffledOrder(candidates[i], rng);
                    searchOneStart(workerConfig, candidates[i], order);
                }
                catch (...)
                {
                    failures[i] = std::current_exception();
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
    for (int i = 1; i < workerCount; ++i)
    {
        if (candidates[i].score > candidates[best].score)
            best = i;
    }
    problem = std::move(candidates[best]);
}

void Solver::solve(BinPackingProblem &problem,
                   optimaru::DecomposedScoreCalculator<BinPackingProblem> &calculator)
{
    requireRewards(config_);
    prepareSolve(problem);
    problem.warmStart = false;  // a seed is used once
    if (!problem.items.empty())
    {
        constructBins(problem, descendingOrder(problem));
        const std::vector<int> start = problem.assignment();
        runEngine(config_, problem, calculator);
        compactIfMoved(problem, start);
    }

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraintTotals() explains exactly this score.
    calculator.resetWorkingSolution(problem);
    problem.score = calculator.calculateScore();
}

}  // namespace bp

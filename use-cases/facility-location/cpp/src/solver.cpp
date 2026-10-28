#include "fl/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "fl/incremental_calculator.hpp"
#include "fl/score_calculator.hpp"
#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace fl
{

namespace
{

// ---------------------------------------------------------------------------
// CloseFacilityMove — the domain's missing macro move
// ---------------------------------------------------------------------------
// facilitySetupCost is a step function of "does this facility serve anybody",
// and it is about half of the soft score. No built-in move can cross that
// step: a facility only stops costing its setup once its LAST consumer
// leaves, so every single-consumer change on the way there is strictly worse
// and the late-acceptance search rejects it. That is why the plain solver
// converges within a tenth of a second and then never improves again.
//
// This operator does the whole step in one move: pick a used facility and
// empty it, either by redistributing its consumers over the facilities that
// are already open (so the move pays no new setup cost at all) or by
// migrating them together to one alternative site. The acceptor then judges
// the complete trade — exactly one score evaluation, not a sequence of
// individually-worse ones.
//
// Cost is O(consumers + facilities) per move (two censuses plus a short walk
// down each moved consumer's distance-ordered facility list), roughly 10x a
// built-in change move; the adaptive roulette wheel decides how often it is
// worth that. Selection between it and the built-in mix is by move outcome.
class CloseFacilityMove final
    : public optimaru::MoveOperator<FacilityLocationProblem>
{
public:
    // `preference` holds, per consumer, the `keep` nearest facilities in
    // ascending distance (flattened consumerIndex * keep + rank).
    CloseFacilityMove(std::shared_ptr<std::vector<int>> preference,
                      int keep,
                      double migrateProbability)
        : preference_(std::move(preference)),
          keep_(keep),
          migrateProbability_(migrateProbability)
    {
    }

    const char *name() const override { return "closeFacility"; }

    bool apply(optimaru::OperatorContext<FacilityLocationProblem> &ctx,
               std::mt19937_64 &rng,
               std::vector<int> &entities) override
    {
        FacilityLocationProblem &problem = ctx.director.workingSolution();
        const int facilityCount = problem.facilityCount();
        const int consumerCount = problem.consumerCount();

        // Census: consumer count and used demand per facility, plus the list
        // of currently used facilities.
        count_.assign(facilityCount, 0);
        load_.assign(facilityCount, 0);
        for (int c = 0; c < consumerCount; ++c)
        {
            const Consumer &consumer = problem.consumers[c];
            if (consumer.facility >= 0)
            {
                ++count_[consumer.facility];
                load_[consumer.facility] += consumer.demand;
            }
        }
        used_.clear();
        for (int f = 0; f < facilityCount; ++f)
        {
            if (count_[f] > 0)
                used_.push_back(f);
        }
        if (used_.size() < 2)
            return false;  // nowhere to move the consumers to

        const int victim = used_[rng() % used_.size()];
        members_.clear();
        for (int c = 0; c < consumerCount; ++c)
        {
            if (problem.consumers[c].facility == victim)
                members_.push_back(c);
        }
        if (members_.empty())
            return false;

        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const bool migrate = unit(rng) < migrateProbability_;

        // The victim is being emptied: its residual capacity is irrelevant
        // and it must never be picked as a target.
        count_[victim] = 0;
        load_[victim] = 0;

        int migrationTarget = -1;
        if (migrate)
        {
            migrationTarget = pickMigrationTarget(problem, victim, rng);
            if (migrationTarget < 0)
                return false;
        }

        lastVictim_ = victim;
        ctx.director.beginVariableChanges();
        for (int c : members_)
        {
            const int target
                = migrate ? migrationTarget : pickTarget(problem, c, victim);
            ctx.director.changeVariable(c, target);
            ++count_[target];
            load_[target] += problem.consumers[c].demand;
            entities.push_back(c);
        }
        ctx.director.endVariableChanges();
        return true;
    }

    void
    undoLast(optimaru::OperatorContext<FacilityLocationProblem> &ctx) override
    {
        ctx.director.beginVariableChanges();
        for (int c : members_)
            ctx.director.changeVariable(c, lastVictim_);
        ctx.director.endVariableChanges();
    }

private:
    bool fits(const FacilityLocationProblem &problem,
              int facility,
              std::int64_t demand) const
    {
        return load_[facility] + demand
               <= problem.facilities[facility].capacity;
    }

    // Where one evicted consumer goes: the closest already-open facility with
    // room (no new setup cost), else the closest facility with room at all,
    // else the closest facility, period.
    int pickTarget(const FacilityLocationProblem &problem,
                   int consumerIndex,
                   int victim) const
    {
        const std::int64_t demand = problem.consumers[consumerIndex].demand;
        const int *order = preference_->data()
                           + static_cast<std::size_t>(consumerIndex) * keep_;
        int roomAnywhere = -1;
        for (int rank = 0; rank < keep_; ++rank)
        {
            const int f = order[rank];
            if (f == victim)
                continue;
            if (!fits(problem, f, demand))
                continue;
            if (count_[f] > 0)
                return f;  // already open: the cheapest kind of target
            if (roomAnywhere < 0)
                roomAnywhere = f;
        }
        if (roomAnywhere >= 0)
            return roomAnywhere;
        return order[0] == victim ? order[1 % keep_] : order[0];
    }

    // Where the whole facility moves: the closest alternative site that can
    // absorb the victim's entire load, seen from one of its consumers.
    int pickMigrationTarget(const FacilityLocationProblem &problem,
                            int victim,
                            std::mt19937_64 &rng) const
    {
        std::int64_t demand = 0;
        for (int c : members_)
            demand += problem.consumers[c].demand;
        const int seed = members_[rng() % members_.size()];
        const int *order
            = preference_->data() + static_cast<std::size_t>(seed) * keep_;
        for (int rank = 0; rank < keep_; ++rank)
        {
            const int f = order[rank];
            if (f != victim && fits(problem, f, demand))
                return f;
        }
        return -1;
    }

    std::shared_ptr<std::vector<int>> preference_;
    int keep_;
    double migrateProbability_;

    // Scratch, reused across moves (no per-move allocation).
    std::vector<int> count_;
    std::vector<std::int64_t> load_;
    std::vector<int> used_;
    std::vector<int> members_;
    int lastVictim_ = -1;
};

// Per consumer, the `keep` nearest facilities in ascending distance
// (flattened consumerIndex * keep + rank): where CloseFacilityMove looks for
// a new home for an evicted consumer.
std::shared_ptr<std::vector<int>>
buildFacilityPreference(const FacilityLocationProblem &problem, int keep)
{
    auto preference = std::make_shared<std::vector<int>>(
        static_cast<std::size_t>(problem.consumerCount()) * keep);
    std::vector<std::pair<std::int64_t, int>> byDistance;
    byDistance.reserve(problem.facilities.size());
    for (int c = 0; c < problem.consumerCount(); ++c)
    {
        byDistance.clear();
        for (int f = 0; f < problem.facilityCount(); ++f)
            byDistance.emplace_back(problem.distanceOf(c, f), f);
        std::partial_sort(
            byDistance.begin(), byDistance.begin() + keep, byDistance.end());
        for (int rank = 0; rank < keep; ++rank)
        {
            (*preference)[static_cast<std::size_t>(c) * keep + rank]
                = byDistance[rank].second;
        }
    }
    return preference;
}

// Shared engine wiring for both solve() overloads: construction heuristic +
// late-acceptance local search (+ ALNS kicks), plus the domain's own macro
// move, driven by whichever calculator the caller brought (the monolithic
// incremental one, or a plugin assembly).
void runEngine(
    const Solver::Config &config,
    FacilityLocationProblem &problem,
    optimaru::IncrementalScoreCalculator<FacilityLocationProblem> &calculator)
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
              : std::clamp<std::size_t>(
                    8 * problem.consumers.size(), 400, 3200);
    engineConfig.localSearch.moveSelector.changeMoveWeight
        = config.changeMoveWeight;
    engineConfig.localSearch.moveSelector.swapMoveWeight
        = config.swapMoveWeight;
    engineConfig.localSearch.customMoveProbability
        = config.customMoveProbability;
    engineConfig.localSearch.descentWeight = config.descentWeight;

    // Nearby selection: consumer-to-consumer Euclidean degree distance (the
    // ordering is what matters, so the sqrt/meter scaling is skipped). The
    // problem outlives the engine run, so capturing it by reference is safe.
    if (config.nearbyProbability > 0.0 && config.nearbySize > 0)
    {
        engineConfig.localSearch.nearby.probability = config.nearbyProbability;
        engineConfig.localSearch.nearby.size = config.nearbySize;
        const FacilityLocationProblem &fixed = problem;
        engineConfig.localSearch.nearby.distanceMeter = [&fixed](int a, int b)
        {
            const Location &la = fixed.consumers[a].location;
            const Location &lb = fixed.consumers[b].location;
            const double dLat = la.latitude - lb.latitude;
            const double dLon = la.longitude - lb.longitude;
            return dLat * dLat + dLon * dLon;
        };
    }

    // Distance-ordered facility shortlist per consumer: where
    // CloseFacilityMove looks for a new home for each evicted consumer.
    const int preferenceKeep
        = std::min(config.closeFacilityKeep, problem.facilityCount());
    std::shared_ptr<std::vector<int>> preference;
    if (config.closeFacilityMoves && preferenceKeep > 0)
        preference = buildFacilityPreference(problem, preferenceKeep);

    // Guided change-move targets: draw the consumer's new facility from its
    // k nearest ones with the configured probability (-1 falls back to the
    // engine's uniform draw).
    std::shared_ptr<std::vector<int>> valuePreference;
    const int valueKeep
        = std::min(config.valueRecommenderKeep, problem.facilityCount());
    if (config.valueRecommenderProbability > 0.0 && valueKeep > 0)
    {
        valuePreference = (valueKeep == preferenceKeep && preference)
                              ? preference
                              : buildFacilityPreference(problem, valueKeep);
        const double p = config.valueRecommenderProbability;
        engineConfig.localSearch.valueRecommender
            = [valuePreference, valueKeep, p](std::mt19937_64 &rng,
                                              int entityIndex) -> int
        {
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            if (unit(rng) >= p)
                return -1;
            const std::size_t base
                = static_cast<std::size_t>(entityIndex) * valueKeep;
            return (*valuePreference)[base + rng() % valueKeep];
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

    optimaru::Solver<FacilityLocationProblem> engine(std::move(engineConfig));
    if (preference)
    {
        engine.addMoveOperator(std::make_unique<CloseFacilityMove>(
            preference,
            preferenceKeep,
            config.closeFacilityMigrateProbability));
    }
    engine.solve(problem, calculator);
}

}  // namespace

void Solver::solve(FacilityLocationProblem &problem)
{
    ScoreCalculator::prepare(problem);  // distance matrix
    if (problem.consumers.empty() || problem.facilities.empty())
    {
        ScoreCalculator::calculateScore(problem);
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }

    // Independent parallel starts (the school-timetabling pattern): worker i
    // solves its own copy with seed + i. The engine's own multi-start cannot
    // be used, because it runs the built-in move mix only — a registered
    // custom operator like CloseFacilityMove is per-Solver and stateful, so
    // it would silently vanish in the worker threads. The best score wins.
    const int workerCount = std::max(1, config_.parallelSolverCount);
    if (workerCount > 1)
    {
        std::vector<FacilityLocationProblem> candidates(workerCount, problem);
        std::vector<std::exception_ptr> failures(workerCount);
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (int i = 0; i < workerCount; ++i)
        {
            workers.emplace_back(
                [this, i, &candidates, &failures]
                {
                    try
                    {
                        Config workerConfig = config_;
                        workerConfig.parallelSolverCount = 1;
                        workerConfig.randomSeed
                            += static_cast<std::uint64_t>(i);
                        if (i != 0)
                            workerConfig.onNewBest = nullptr;
                        Solver worker(std::move(workerConfig));
                        worker.solve(candidates[i]);
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
        return;
    }

    FacilityLocationIncrementalScoreCalculator calculator;
    runEngine(config_, problem, calculator);

    // The authoritative final score: a full recompute on the best solution.
    problem.score = ScoreCalculator::calculateScore(problem);
}

void Solver::solve(
    FacilityLocationProblem &problem,
    optimaru::DecomposedScoreCalculator<FacilityLocationProblem> &calculator)
{
    ScoreCalculator::prepare(problem);  // distance matrix
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }
    if (!problem.consumers.empty() && !problem.facilities.empty())
        runEngine(config_, problem, calculator);

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraintTotals() explains exactly this score.
    calculator.resetWorkingSolution(problem);
    problem.score = calculator.calculateScore();
}

}  // namespace fl

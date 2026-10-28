#include "st/canonical_solver.hpp"

#include <algorithm>
#include <exception>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "st/canonical_incremental_calculator.hpp"
#include "st/canonical_score_calculator.hpp"
#include "st/moves/hot_source.hpp"
#include "st/moves/reassign.hpp"
#include "st/moves/smart_class_swap.hpp"
#include "st/moves/unassign.hpp"
#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"

namespace st::canon
{

namespace
{

// Guided-search tables shared by both solve() overloads; the smart operator
// needs them after the engine config is built.
struct GuidedSetup
{
    std::shared_ptr<std::vector<std::vector<int>>> allowedSlots;
    std::shared_ptr<std::vector<int>> classOf;
    std::vector<std::vector<int>> lessonsOfClass;
};

// Everything of the engine configuration that does not depend on which
// calculator drives the search: termination, late acceptance, the move mix,
// the adaptive LNS, allowed-slot value guidance, same-class nearby
// neighborhoods and the FFD construction order. The entity recommender
// (violation-guided selection) binds to the concrete calculator, so each
// solve() overload wires it separately.
GuidedSetup configureEngine(const Solver::Config &config,
                            const Timetable &table,
                            optimaru::SolverConfig &engineConfig)
{
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
              : std::clamp<std::size_t>(4 * table.lessons.size(), 400, 3200);
    engineConfig.localSearch.moveSelector.changeMoveWeight
        = config.changeMoveWeight;
    engineConfig.localSearch.moveSelector.swapMoveWeight
        = config.swapMoveWeight;
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
    engineConfig.localSearch.descentWeight = config.descentWeight;
    engineConfig.localSearch.descentMaxSweeps = config.descentMaxSweeps;

    GuidedSetup setup;

    // Guided value selection: change moves only draw slots the lesson is
    // allowed on (its allowedSlot table), so the search never wanders into
    // H5 territory. Lessons allowed nowhere fall back to uniform-random (-1).
    setup.allowedSlots = std::make_shared<std::vector<std::vector<int>>>();
    setup.allowedSlots->reserve(table.lessons.size());
    for (const Lesson &lesson : table.lessons)
    {
        std::vector<int> slots;
        for (int s = 0; s < table.slotCount(); ++s)
        {
            if (Timetable::slotAllowed(lesson, s))
                slots.push_back(s);
        }
        setup.allowedSlots->push_back(std::move(slots));
    }
    const std::shared_ptr<std::vector<std::vector<int>>> &allowedSlots
        = setup.allowedSlots;
    engineConfig.localSearch.valueRecommender
        = [allowedSlots](std::mt19937_64 &rng, int lessonIndex) -> int
    {
        const std::vector<int> &slots = (*allowedSlots)[lessonIndex];
        if (slots.empty())
            return -1;
        return slots[rng() % slots.size()];
    };

    // Class membership tables, shared by the nearby meter and the smart
    // same-class swap operator.
    setup.classOf = std::make_shared<std::vector<int>>();
    setup.classOf->reserve(table.lessons.size());
    setup.lessonsOfClass.assign(std::max(1, table.classCount), {});
    for (int i = 0; i < static_cast<int>(table.lessons.size()); ++i)
    {
        const int classGroup = table.lessons[i].classGroupId;
        setup.classOf->push_back(classGroup);
        setup.lessonsOfClass[classGroup].push_back(i);
    }

    // Same-class nearby neighborhoods (see Config::sameClassNearby). The
    // neighbor list of a lesson is its classmates (distance 0) up to the
    // largest class size, so a swap partner drawn nearby always shares the
    // class and the LNS related ruin removes one class's cluster.
    if (config.sameClassNearby)
    {
        std::size_t maxClassSize = 0;
        for (const std::vector<int> &members : setup.lessonsOfClass)
            maxClassSize = std::max(maxClassSize, members.size());
        const std::shared_ptr<std::vector<int>> &classOf = setup.classOf;
        engineConfig.localSearch.nearby.distanceMeter = [classOf](int a, int b)
        { return (*classOf)[a] == (*classOf)[b] ? 0.0 : 1.0; };
        engineConfig.localSearch.nearby.size
            = std::max(1, static_cast<int>(maxClassSize) - 1);
        engineConfig.localSearch.nearby.probability
            = config.sameClassNearbyProbability;
    }

    // First Fit Decreasing: initialize the most constrained lessons first —
    // block members, team teaching / merged lessons (many teachers), special
    // room demand, and lessons with few allowed slots.
    auto difficulty = std::make_shared<std::vector<double>>();
    difficulty->reserve(table.lessons.size());
    for (std::size_t i = 0; i < table.lessons.size(); ++i)
    {
        const Lesson &lesson = table.lessons[i];
        int demand = 0;
        for (int d : lesson.roomTypeDemand)
            demand += d;
        (*difficulty)
            .push_back(
                (lesson.blockGroup >= 0 ? 100.0 : 0.0)
                + 10.0 * static_cast<double>(lesson.teachers.size())
                + 5.0 * demand
                + static_cast<double>(table.slotCount()
                                      - (*setup.allowedSlots)[i].size()));
    }
    engineConfig.constructionHeuristic.entityDifficulty
        = [difficulty](int lessonIndex) { return (*difficulty)[lessonIndex]; };

    return setup;
}

}  // namespace

void Solver::solve(Timetable &table)
{
    table.weights = config_.weights;
    ScoreCalculator::prepare(table);  // normalize ids / derived tables
    if (table.lessons.empty() || table.slotCount() == 0)
    {
        ScoreCalculator::calculateScore(table);
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }

    // Independent parallel starts: worker i
    // solves its own copy with seed + i and a per-worker calculator — the
    // guided-selection hooks below read live calculator state, so the engine's
    // built-in multi-start (which shares the config across threads) cannot be
    // used. The best final score wins.
    const int workerCount = std::max(1, config_.parallelSolverCount);
    if (workerCount > 1)
    {
        std::vector<Timetable> candidates(workerCount, table);
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
        table = std::move(candidates[best]);
        return;
    }

    optimaru::SolverConfig engineConfig;
    const GuidedSetup setup = configureEngine(config_, table, engineConfig);

    // The single calculator instance of this (single-start) solve. Created
    // before the engine so the guided-selection hooks can bind to it; the
    // engine resets it on the working solution at solve start.
    CanonicalIncrementalScoreCalculator calculator;

    // Violation-guided entity selection (the es guided-moves pattern): with
    // probability hotLessonProbability the moved lesson is drawn from the
    // lessons currently involved in a hard violation. Once feasible there are
    // no hot lessons, so this falls back to uniform exploration by itself.
    if (config_.hotLessonProbability > 0.0)
    {
        const double probability = config_.hotLessonProbability;
        engineConfig.localSearch.entityRecommender
            = [&calculator, probability](std::mt19937_64 &rng) -> int
        {
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            if (unit(rng) >= probability)
                return -1;
            return calculator.hotLesson(rng);
        };
    }

    // Construction heuristic + late-acceptance local search (+ ALNS kicks).
    optimaru::Solver<Timetable> engine(std::move(engineConfig));
    if (config_.smartClassSwapMoves)
    {
        engine.addMoveOperator(
            std::make_unique<
                SmartClassSwapMove<CanonicalIncrementalScoreCalculator>>(
                setup.lessonsOfClass,
                *setup.classOf,
                setup.allowedSlots,
                config_.smartClassSwapCandidates,
                &calculator));
    }
    engine.solve(table, calculator);

    // The authoritative final score: a full recompute on the best solution.
    table.score = ScoreCalculator::calculateScore(table);
}

void Solver::solve(Timetable &table,
                   optimaru::DecomposedScoreCalculator<Timetable> &calculator)
{
    ScoreCalculator::prepare(table);  // normalize ids / derived tables
    if (table.lessons.empty() || table.slotCount() == 0)
    {
        calculator.resetWorkingSolution(table);
        table.score = calculator.calculateScore();
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }

    optimaru::SolverConfig engineConfig;
    const GuidedSetup setup = configureEngine(config_, table, engineConfig);

    // Violation-guided entity selection, fed by the decomposed calculator's
    // penalized units instead of the monolithic hot sets (see
    // DecomposedHotSource). Falls back to uniform exploration once feasible.
    DecomposedHotSource hotSource(&calculator);
    if (config_.hotLessonProbability > 0.0)
    {
        const double probability = config_.hotLessonProbability;
        engineConfig.localSearch.entityRecommender
            = [&hotSource, probability](std::mt19937_64 &rng) -> int
        {
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            if (unit(rng) >= probability)
                return -1;
            return hotSource.hotLesson(rng);
        };
    }

    optimaru::Solver<Timetable> engine(std::move(engineConfig));
    if (config_.smartClassSwapMoves)
    {
        engine.addMoveOperator(
            std::make_unique<SmartClassSwapMove<DecomposedHotSource>>(
                setup.lessonsOfClass,
                *setup.classOf,
                setup.allowedSlots,
                config_.smartClassSwapCandidates,
                &hotSource));
    }
    // Only worth a slot in the move mix when the assembly prices unassignment
    // as a soft penalty; with H1 hard, unassigning is always a hard loss.
    if (allowsUnassigned(calculator))
    {
        engine.addMoveOperator(
            std::make_unique<UnassignMove<DecomposedHotSource>>(&hotSource));
        // The way back in. Without it the search only ever drops (dropping is
        // guided by the hot lessons, re-placing is not) and the run ends with
        // far more unassigned than the instance requires.
        engine.addMoveOperator(
            std::make_unique<ReassignMove<DecomposedHotSource>>(
                setup.allowedSlots, config_.reassignCandidates));
    }
    engine.solve(table, calculator);

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraintTotals() explains exactly this score (the es pattern).
    calculator.resetWorkingSolution(table);
    table.score = calculator.calculateScore();
}

}  // namespace st::canon

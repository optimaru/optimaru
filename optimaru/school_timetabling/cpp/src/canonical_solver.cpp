#include "st/canonical_solver.hpp"

#include <algorithm>
#include <exception>
#include <memory>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "st/canonical_incremental_calculator.hpp"
#include "st/canonical_score_calculator.hpp"
#include "st/moves/hot_source.hpp"
#include "st/moves/reassign.hpp"
#include "st/moves/smart_class_swap.hpp"
#include "st/moves/unassign.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/worker_threads.hpp"

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
                            optimaru::Settings &engineConfig)
{
    engineConfig.seed = config.seed;
    engineConfig.score_checks = config.score_checks;
    engineConfig.on_new_best = config.on_new_best;
    engineConfig.limits.time_limit = config.time_limit;
    engineConfig.limits.target_score = config.target_score;
    engineConfig.limits.stall_time_limit = config.stall_time_limit;
    engineConfig.limits.step_limit = config.step_limit;
    engineConfig.limits.stall_step_limit
        = config.stall_step_limit;
    // history_length 0 = auto-scale with the instance (see Config).
    engineConfig.search.acceptance.history_length
        = config.history_length != 0
              ? config.history_length
              : std::clamp<std::size_t>(4 * table.lessons.size(), 400, 3200);
    engineConfig.search.moves.change_weight
        = config.change_weight;
    engineConfig.search.moves.swap_weight
        = config.swap_weight;
    auto &lns = engineConfig.search.ruin_recreate;
    lns.enabled = config.ruin_recreate;
    lns.unimproved_step_trigger = config.ruinRecreateUnimprovedStepTrigger;
    lns.min_ruin_fraction = config.ruinRecreateMinRuinFraction;
    lns.max_ruin_fraction = config.ruinRecreateMaxRuinFraction;
    lns.max_ruin_count = config.ruinRecreateMaxRuinCount;
    lns.operator_decay = config.ruinRecreateOperatorDecay;
    lns.rewards = config.ruinRecreateRewards;
    lns.polish_steps = config.ruinRecreatePolishSteps;
    lns.as_move_weight = config.ruinRecreateAsMoveWeight;
    lns.as_move_max_ruin = config.ruinRecreateAsMoveMaxRuin;
    engineConfig.search.descent_weight = config.descent_weight;
    engineConfig.search.descent_max_sweeps = config.descent_max_sweeps;

    GuidedSetup setup;

    // Guided value selection: change moves only draw slots the lesson is
    // allowed on (its allowedSlot table), so the search never wanders into
    // disallowedSlotCost territory. Lessons allowed nowhere fall back to
    // uniform-random (-1).
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
    engineConfig.search.value_hint
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
        engineConfig.search.nearby.distance = [classOf](int a, int b)
        { return (*classOf)[a] == (*classOf)[b] ? 0.0 : 1.0; };
        engineConfig.search.nearby.size
            = std::max(1, static_cast<int>(maxClassSize) - 1);
        engineConfig.search.nearby.probability
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
    engineConfig.construction.difficulty
        = [difficulty](int lessonIndex) { return (*difficulty)[lessonIndex]; };

    return setup;
}

}  // namespace

void Solver::solve(Timetable &table)
{
    ScoreCalculator::prepare(table);  // normalize ids / derived tables
    if (table.lessons.empty() || table.slotCount() == 0)
    {
        ScoreCalculator::score(table, config_.weights);
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
        // Raised by the first worker that fails: every worker's engine checks
        // it once a step, so the others stop instead of using up their budget.
        auto stop = std::make_shared<std::atomic<bool>>(false);
        // A worker that cannot be started (std::system_error) stops and joins
        // the ones already running before the error is rethrown here.
        optimaru::run_worker_threads(
            workerCount, *stop,
            [this, &candidates, &failures, &stop](int i)
            {
                try
                {
                    Config workerConfig = config_;
                    workerConfig.parallelSolverCount = 1;
                    workerConfig.stop_flag = stop;
                    workerConfig.seed
                        += static_cast<std::uint64_t>(i);
                    if (i != 0)
                        workerConfig.on_new_best = nullptr;
                    Solver worker(std::move(workerConfig));
                    worker.solve(candidates[i]);
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
        table = std::move(candidates[best]);
        return;
    }

    optimaru::Settings engineConfig;
    const GuidedSetup setup = configureEngine(config_, table, engineConfig);

    // The single calculator instance of this (single-start) solve. Created
    // before the engine so the guided-selection hooks can bind to it; the
    // engine resets it on the working solution at solve start.
    CanonicalIncrementalScoreCalculator calculator(config_.weights);

    // Violation-guided entity selection (the es guided-moves pattern): with
    // probability hotLessonProbability the moved lesson is drawn from the
    // lessons currently involved in a hard violation. Once feasible there are
    // no hot lessons, so this falls back to uniform exploration by itself.
    if (config_.hotLessonProbability > 0.0)
    {
        const double probability = config_.hotLessonProbability;
        engineConfig.search.entity_hint
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
    if (config_.stop_flag)
        engine.share_stop_flag(config_.stop_flag);
    if (config_.smartClassSwapMoves)
    {
        engine.add_move_operator(
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
    table.score = ScoreCalculator::score(table, config_.weights);
}

void Solver::solve(Timetable &table,
                   optimaru::DecomposedObjective<Timetable> &calculator)
{
    ScoreCalculator::prepare(table);  // normalize ids / derived tables
    if (table.lessons.empty() || table.slotCount() == 0)
    {
        calculator.rebuild(table);
        table.score = calculator.score();
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }

    optimaru::Settings engineConfig;
    const GuidedSetup setup = configureEngine(config_, table, engineConfig);

    // Violation-guided entity selection, fed by the decomposed calculator's
    // penalized units instead of the monolithic hot sets (see
    // DecomposedHotSource). Falls back to uniform exploration once feasible.
    DecomposedHotSource hotSource(&calculator);
    if (config_.hotLessonProbability > 0.0)
    {
        const double probability = config_.hotLessonProbability;
        engineConfig.search.entity_hint
            = [&hotSource, probability](std::mt19937_64 &rng) -> int
        {
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            if (unit(rng) >= probability)
                return -1;
            return hotSource.hotLesson(rng);
        };
    }

    optimaru::Solver<Timetable> engine(std::move(engineConfig));
    if (config_.stop_flag)
        engine.share_stop_flag(config_.stop_flag);
    if (config_.smartClassSwapMoves)
    {
        engine.add_move_operator(
            std::make_unique<SmartClassSwapMove<DecomposedHotSource>>(
                setup.lessonsOfClass,
                *setup.classOf,
                setup.allowedSlots,
                config_.smartClassSwapCandidates,
                &hotSource));
    }
    // Only worth a slot in the move mix when the assembly prices unassignment
    // as a soft penalty; with unassignedLessonsCost hard, unassigning is
    // always a hard loss.
    if (allowsUnassigned(calculator))
    {
        engine.add_move_operator(
            std::make_unique<UnassignMove<DecomposedHotSource>>(&hotSource));
        // The way back in. Without it the search only ever drops (dropping is
        // guided by the hot lessons, re-placing is not) and the run ends with
        // far more unassigned than the instance requires.
        engine.add_move_operator(
            std::make_unique<ReassignMove<DecomposedHotSource>>(
                setup.allowedSlots, config_.reassignCandidates));
    }
    engine.solve(table, calculator);

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraint_totals() explains exactly this score (the es pattern).
    calculator.rebuild(table);
    table.score = calculator.score();
}

}  // namespace st::canon

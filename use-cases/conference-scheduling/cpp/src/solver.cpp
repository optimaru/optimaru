#include "cs/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "cs/incremental_calculator.hpp"
#include "cs/score_calculator.hpp"
#include "optimaru/api/solver/solver_factory.hpp"
#include "optimaru/config/solver/solver_config.hpp"

namespace cs
{

namespace
{

// Shared engine wiring for both solve() overloads: termination, late
// acceptance, the move mix, the ALNS knobs and whatever guided selection the
// domain config asks for.
optimaru::SolverConfig buildEngineConfig(const Solver::Config &config,
                                         const ConferenceSchedule &schedule)
{
    optimaru::SolverConfig engineConfig;
    engineConfig.randomSeed = config.randomSeed;
    engineConfig.environmentMode = config.environmentMode;
    engineConfig.multiStartCount = 1;  // parallel starts are fanned out above
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
              : std::clamp<std::size_t>(8 * schedule.talks.size(), 800, 3200);
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
    // asMoveWeight < 0 = auto: the budget-share rule (weight * valueRange
    // ~ 0.2; see Config).
    lns.asMoveWeight = config.ruinRecreateAsMoveWeight >= 0.0
                           ? config.ruinRecreateAsMoveWeight
                           : 0.2 / std::max(1, schedule.slotRoomCount());
    lns.asMoveMaxRuin = config.ruinRecreateAsMoveMaxRuin;
    engineConfig.localSearch.descentWeight = config.descentWeight;

    // Per-talk type-compatible (timeslot, room) cells: the stand-in for the
    // quickstart's per-TalkType value ranges. Change moves draw from this
    // list with compatibleValueProbability; the construction heuristic
    // additionally initializes the most constrained talks first.
    const int slotCount = schedule.timeslotCount();
    const int roomCount = schedule.roomCount();
    auto compatibleValues = std::make_shared<std::vector<std::vector<int>>>();
    compatibleValues->reserve(schedule.talks.size());
    for (const Talk &talk : schedule.talks)
    {
        std::vector<int> values;
        for (int s = 0; s < slotCount; ++s)
        {
            if (!ScoreCalculator::typeAllowsSlot(schedule, talk.talkType, s))
                continue;
            for (int r = 0; r < roomCount; ++r)
            {
                if (ScoreCalculator::typeAllowsRoom(schedule, talk.talkType, r))
                    values.push_back(schedule.slotRoomOf(s, r));
            }
        }
        compatibleValues->push_back(std::move(values));
    }
    engineConfig.constructionHeuristic.entityDifficulty
        = [compatibleValues](int talkIndex)
    {
        // Fewer compatible cells == harder; First Fit Decreasing.
        return -static_cast<double>((*compatibleValues)[talkIndex].size());
    };
    if (config.compatibleValueProbability > 0.0)
    {
        const double probability = config.compatibleValueProbability;
        engineConfig.localSearch.valueRecommender
            = [compatibleValues, probability](std::mt19937_64 &rng,
                                              int talkIndex) -> int
        {
            const std::vector<int> &values = (*compatibleValues)[talkIndex];
            if (values.empty())
                return -1;  // fall back to the uniform draw
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            if (unit(rng) >= probability)
                return -1;
            return values[rng() % values.size()];
        };
    }

    return engineConfig;
}

}  // namespace

void Solver::solve(ConferenceSchedule &schedule)
{
    ScoreCalculator::prepare(schedule);  // intern tags / flatten tables
    if (schedule.talks.empty() || schedule.slotRoomCount() == 0)
    {
        ScoreCalculator::calculateScore(schedule);
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }

    // Independent parallel starts, fanned out here rather than through the
    // engine's built-in multi-start: the budget is driven from this side (see
    // the restart segments below), and the engine's multi-start would instead
    // hand every thread one unsegmented full-budget run. Worker i solves its
    // own copy with randomSeed + i; the best final score wins. (The
    // school-timetabling canonical solver fans out here for the same reason.)
    const int workerCount = std::max(1, config_.parallelSolverCount);
    if (workerCount > 1)
    {
        std::vector<ConferenceSchedule> candidates(workerCount, schedule);
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
        schedule = std::move(candidates[best]);
        return;
    }

    optimaru::SolverConfig engineConfig = buildEngineConfig(config_, schedule);

    // Construction heuristic + late-acceptance local search (+ ALNS kicks),
    // run as `restartSegments` consecutive segments over the one wall-clock
    // budget (iterated local search).
    //
    // Why segment at all: on this domain a single local-search run stops
    // paying long before the budget does. On the scaled quality-bench
    // instance the last new best lands around 3s of a 10s solve -- the
    // late-acceptance history has filled with the incumbent's own score and
    // the ALNS kick interval, which doubles after every kick that fails to
    // find a new best, has backed off past the remaining step count. Both are
    // per-run state, so restarting the phase resets them. Each segment starts
    // from the previous segment's best (the construction heuristic only
    // touches unassigned talks, and local search restores its best snapshot
    // before returning), so the sequence is monotone: a segment can only add
    // to the incumbent.
    //
    // Segmenting is skipped whenever a step- or unimproved-based termination
    // is configured, since those count per run and segmenting would silently
    // multiply them.
    const bool segmentable = config_.stepCountLimit == 0
                             && config_.unimprovedStepCountLimit == 0
                             && config_.unimprovedSpentLimit.count() == 0
                             && config_.spentLimit.count() > 0;
    const int segments = segmentable ? std::max(1, config_.restartSegments) : 1;
    const auto deadline = std::chrono::steady_clock::now() + config_.spentLimit;

    // The engine's step counter restarts with each segment; keep the
    // reported step numbers monotone across segments for onNewBest
    // consumers.
    std::int64_t stepOffset = 0;
    std::int64_t lastStep = 0;
    Score best = schedule.score;
    // One calculator for every segment (the engine resets it on the working
    // solution at each solve start), so the violation-guided entity
    // recommender below can bind to its live hot-talk view.
    ConferenceSchedulingIncrementalScoreCalculator calculator;
    for (int segment = 0; segment < segments; ++segment)
    {
        optimaru::SolverConfig segmentConfig = engineConfig;
        if (segments > 1)
        {
            const auto now = std::chrono::steady_clock::now();
            const auto remaining
                = std::chrono::duration_cast<std::chrono::milliseconds>(deadline
                                                                        - now);
            if (segment > 0 && remaining.count() <= 0)
                break;
            segmentConfig.termination.spentLimit = std::max(
                std::chrono::milliseconds(1), remaining / (segments - segment));
            // A distinct stream per segment, kept far away from the
            // parallel-worker offsets above.
            segmentConfig.randomSeed
                = config_.randomSeed
                  + static_cast<std::uint64_t>(segment) * 0x9E3779B97F4A7C15ULL;
        }
        segmentConfig.onNewBest
            = [&lastStep, &stepOffset, this](std::int64_t step, const Score &sc)
        {
            lastStep = step;
            if (config_.onNewBest)
                config_.onNewBest(stepOffset + step, sc);
        };
        // Violation-guided entity selection (see Config::hotTalkProbability):
        // rejection-sample a few uniform draws against the calculator's O(1)
        // hot-talk check; -1 falls back to the uniform draw.
        if (config_.hotTalkProbability > 0.0)
        {
            const double probability = config_.hotTalkProbability;
            const int talkCount = static_cast<int>(schedule.talks.size());
            segmentConfig.localSearch.entityRecommender
                = [&calculator, probability, talkCount](
                      std::mt19937_64 &rng) -> int
            {
                std::uniform_real_distribution<double> unit(0.0, 1.0);
                if (unit(rng) >= probability)
                    return -1;
                for (int attempt = 0; attempt < 4; ++attempt)
                {
                    const int talk = static_cast<int>(
                        rng() % static_cast<std::uint64_t>(talkCount));
                    if (calculator.isHotTalk(talk))
                        return talk;
                }
                return -1;
            };
        }

        auto engine = optimaru::SolverFactory<ConferenceSchedule>::create(
                          std::move(segmentConfig))
                          .buildSolver();
        lastStep = 0;
        best = engine.solve(schedule, calculator);
        stepOffset += lastStep + 1;
        if (best >= config_.bestScoreLimit)
            break;
    }

    // The authoritative final score: a full recompute on the best solution.
    schedule.score = ScoreCalculator::calculateScore(schedule);
}

void Solver::solve(
    ConferenceSchedule &schedule,
    optimaru::DecomposedScoreCalculator<ConferenceSchedule> &calculator)
{
    ScoreCalculator::prepare(schedule);
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }
    if (!schedule.talks.empty() && schedule.slotRoomCount() != 0)
    {
        // One segment, one start: both the segmented restarts and the
        // parallel workers need a calculator of their own, and a
        // caller-assembled plugin cannot be cloned.
        auto engine = optimaru::SolverFactory<ConferenceSchedule>::create(
                          buildEngineConfig(config_, schedule))
                          .buildSolver();
        engine.solve(schedule, calculator);
    }

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraintTotals() explains exactly this score.
    calculator.resetWorkingSolution(schedule);
    schedule.score = calculator.calculateScore();
}

}  // namespace cs

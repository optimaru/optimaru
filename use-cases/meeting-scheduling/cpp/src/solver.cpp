#include "ms/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ms/incremental_calculator.hpp"
#include "ms/score_calculator.hpp"
#include "optimaru/api/solver/solver_factory.hpp"
#include "optimaru/config/solver/solver_config.hpp"

namespace ms
{

namespace
{

// Per-assignment "day-fit" starting grain positions: the positions at which
// the meeting's whole [start, start + duration) interval still lands on
// existing grains of the start grain's own day — exactly the starts that
// satisfy hard 2 (avoidOvertime) and hard 5 (startAndEndOnSameDay). The
// composite value range is (grain x room), so these are the grain halves a
// guided change move should prefer; the room half stays uniform because every
// room is legal (only its capacity is penalized, at soft/hard 4 weight 1).
std::vector<std::vector<int>> buildDayFitStarts(const MeetingSchedule &schedule)
{
    const int grainCount = schedule.timeGrainCount();
    std::vector<std::vector<int>> starts(schedule.meetingAssignments.size());
    for (std::size_t a = 0; a < schedule.meetingAssignments.size(); ++a)
    {
        const Meeting &meeting
            = schedule.meetings[schedule.meetingAssignments[a].meeting];
        const int duration = meeting.durationInGrains;
        for (int pos = 0; pos < grainCount; ++pos)
        {
            const TimeGrain &grain = schedule.timeGrains[pos];
            const int lastOffset
                = grain.grainIndex - schedule.grainIndexBase + duration - 1;
            if (lastOffset >= schedule.occupancySpan)
                continue;
            const int total = schedule.grainCountAtOffset(lastOffset);
            if (total == 0)  // overtime (hard 2)
                continue;
            if (schedule.grainCountAtOffsetOnDay(lastOffset, grain.dayOfYear)
                != total)  // crosses a day border (hard 5)
                continue;
            starts[a].push_back(pos);
        }
    }
    return starts;
}

}  // namespace

namespace
{

// Shared engine wiring for both solve() overloads: termination, late
// acceptance, the move mix, the ALNS knobs and the day-fit value recommender
// (which reads only problem facts, so it applies to the plugin path too),
// driven by whichever calculator the caller brought.
void runEngine(
    const Solver::Config &config,
    MeetingSchedule &schedule,
    optimaru::IncrementalScoreCalculator<MeetingSchedule> &calculator,
    int multiStartCount)
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
                    128 * schedule.meetingAssignments.size(), 3200, 6400);
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

    // Guided value selection over the composite (grain x room) value range:
    // with probability dayFitValueProbability a change move draws its start
    // from the meeting's day-fit grains, so the search never wanders into the
    // hard 2 (overtime) / hard 5 (day crossing) region; the room half stays
    // uniform because every room is legal. Returning -1 (hook disabled, or the
    // remaining probability mass) falls back to the engine's uniform draw,
    // which is what keeps the day-crossing states available as stepping
    // stones — forcing day-fit on every draw measured worse than 0.8.
    // The hook reads only problem facts (room count, the precomputed start
    // lists), never a planning variable, so the engine's multi-start — which
    // copies this config into every worker thread while the workers mutate
    // their own solution copies — stays safe. A hook reading live variables
    // would have to move to hand-rolled workers (the school-timetabling
    // pattern).
    std::vector<std::vector<int>> dayFitStarts;
    if (config.dayFitValueProbability > 0.0)
    {
        dayFitStarts = buildDayFitStarts(schedule);
        const double dayFitProbability = config.dayFitValueProbability;
        const MeetingSchedule *working = &schedule;
        const std::vector<std::vector<int>> *starts = &dayFitStarts;
        engineConfig.localSearch.valueRecommender
            = [working, starts, dayFitProbability](std::mt19937_64 &rng,
                                                   int assignmentIndex) -> int
        {
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            if (unit(rng) >= dayFitProbability)
                return -1;
            const std::vector<int> &fit = (*starts)[assignmentIndex];
            if (fit.empty())
                return -1;
            const int roomCount = working->roomCount();
            return working->grainRoomOf(
                fit[rng() % fit.size()],
                static_cast<int>(rng()
                                 % static_cast<std::uint64_t>(roomCount)));
        };
    }

    engineConfig.multiStartCount = multiStartCount;
    auto engine = optimaru::SolverFactory<MeetingSchedule>::create(
                      std::move(engineConfig))
                      .buildSolver();
    engine.solve(schedule, calculator);
}

}  // namespace

void Solver::solve(MeetingSchedule &schedule)
{
    ScoreCalculator::prepare(schedule);  // build the derived tables
    if (schedule.meetingAssignments.empty() || schedule.grainRoomCount() == 0)
    {
        ScoreCalculator::calculateScore(schedule);
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }

    MeetingSchedulingIncrementalScoreCalculator calculator;
    runEngine(config_,
              schedule,
              calculator,
              std::max(1, config_.parallelSolverCount));

    // The authoritative final score: a full recompute on the best solution.
    schedule.score = ScoreCalculator::calculateScore(schedule);
}

void Solver::solve(
    MeetingSchedule &schedule,
    optimaru::DecomposedScoreCalculator<MeetingSchedule> &calculator)
{
    ScoreCalculator::prepare(schedule);
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }
    if (!schedule.meetingAssignments.empty() && schedule.grainRoomCount() != 0)
    {
        // Single-start: the caller-owned calculator cannot be cloned, so the
        // multi-start workers would have nothing to score with.
        runEngine(config_, schedule, calculator, 1);
    }

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraintTotals() explains exactly this score.
    calculator.resetWorkingSolution(schedule);
    schedule.score = calculator.calculateScore();
}

}  // namespace ms

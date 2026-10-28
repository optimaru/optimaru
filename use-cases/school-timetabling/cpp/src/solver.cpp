#include "st/solver.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "st/incremental_calculator.hpp"
#include "st/score_calculator.hpp"
#include "optimaru/api/solver/solver_factory.hpp"
#include "optimaru/config/solver/solver_config.hpp"

namespace st
{

void Solver::solve(Timetable &table)
{
    ScoreCalculator::prepare(table);  // intern ids / build slot adjacency
    if (table.lessons.empty() || table.slotRoomCount() == 0)
    {
        ScoreCalculator::calculateScore(table);
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }

    optimaru::SolverConfig engineConfig;
    engineConfig.randomSeed = config_.randomSeed;
    engineConfig.environmentMode = config_.environmentMode;
    engineConfig.multiStartCount = std::max(1, config_.parallelSolverCount);
    engineConfig.onNewBest = config_.onNewBest;
    engineConfig.termination.spentLimit = config_.spentLimit;
    engineConfig.termination.bestScoreLimit = config_.bestScoreLimit;
    engineConfig.termination.unimprovedSpentLimit
        = config_.unimprovedSpentLimit;
    engineConfig.termination.stepCountLimit = config_.stepCountLimit;
    engineConfig.termination.unimprovedStepCountLimit
        = config_.unimprovedStepCountLimit;
    // lateAcceptanceSize 0 = auto-scale with the instance (see Config).
    engineConfig.localSearch.acceptor.lateAcceptanceSize
        = config_.lateAcceptanceSize != 0
              ? config_.lateAcceptanceSize
              : std::clamp<std::size_t>(8 * table.lessons.size(), 400, 3200);
    engineConfig.localSearch.moveSelector.changeMoveWeight
        = config_.changeMoveWeight;
    engineConfig.localSearch.moveSelector.swapMoveWeight
        = config_.swapMoveWeight;
    auto &lns = engineConfig.localSearch.ruinRecreate;
    lns.enabled = config_.ruinRecreate;
    lns.unimprovedStepTrigger = config_.ruinRecreateUnimprovedStepTrigger;
    lns.minRuinFraction = config_.ruinRecreateMinRuinFraction;
    lns.maxRuinFraction = config_.ruinRecreateMaxRuinFraction;
    lns.maxRuinCount = config_.ruinRecreateMaxRuinCount;
    lns.operatorDecay = config_.ruinRecreateOperatorDecay;
    lns.rewards = config_.ruinRecreateRewards;
    lns.polishSteps = config_.ruinRecreatePolishSteps;
    // AUTO as-move weight (negative sentinel): budget-share 0.2 inside the
    // measured [900, 1300]-cell pay-off window, off outside it (see Config).
    if (config_.ruinRecreateAsMoveWeight < 0.0)
    {
        const int cells = table.slotRoomCount();
        lns.asMoveWeight = (cells >= 900 && cells <= 1300) ? 0.2 / cells : 0.0;
    }
    else
    {
        lns.asMoveWeight = config_.ruinRecreateAsMoveWeight;
    }
    lns.asMoveMaxRuin = config_.ruinRecreateAsMoveMaxRuin;
    engineConfig.localSearch.descentWeight = config_.descentWeight;
    engineConfig.localSearch.descentMaxSweeps = config_.descentMaxSweeps;

    // Same-group nearby neighborhoods (see Config::sameGroupNearby): the
    // groupId is interned by prepare().
    if (config_.sameGroupNearby)
    {
        auto groupOf = std::make_shared<std::vector<int>>();
        groupOf->reserve(table.lessons.size());
        std::vector<std::size_t> groupSize(std::max(1, table.groupCount), 0);
        for (const Lesson &lesson : table.lessons)
        {
            groupOf->push_back(lesson.groupId);
            ++groupSize[lesson.groupId];
        }
        const std::size_t maxGroupSize
            = *std::max_element(groupSize.begin(), groupSize.end());
        engineConfig.localSearch.nearby.distanceMeter = [groupOf](int a, int b)
        { return (*groupOf)[a] == (*groupOf)[b] ? 0.0 : 1.0; };
        engineConfig.localSearch.nearby.size
            = std::max(1, static_cast<int>(maxGroupSize) - 1);
        engineConfig.localSearch.nearby.probability
            = config_.sameGroupNearbyProbability;
    }

    // Construction heuristic + late-acceptance local search (+ ALNS kicks),
    // multi-start parallel when parallelSolverCount > 1. Each start gets its
    // own calculator instance (the calculators are stateful); the shared
    // nearby meter above is a pure read of the classOf table, so it is safe
    // across the multi-start threads.
    auto engine
        = optimaru::SolverFactory<Timetable>::create(std::move(engineConfig))
              .buildSolver();
    engine.solveMultiStart(
        table,
        []
        {
            return std::make_unique<
                SchoolTimetablingIncrementalScoreCalculator>();
        });

    // The authoritative final score: a full recompute on the best solution.
    table.score = ScoreCalculator::calculateScore(table);
}

}  // namespace st

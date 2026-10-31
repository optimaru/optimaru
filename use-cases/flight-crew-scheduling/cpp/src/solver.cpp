#include "fcs/solver.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#include "fcs/incremental_calculator.hpp"
#include "fcs/score_calculator.hpp"
#include "optimaru/api/solver/solver_factory.hpp"
#include "optimaru/config/solver/solver_config.hpp"

namespace fcs
{

namespace
{

// Translates a domain Config into the engine's SolverConfig: termination,
// late acceptance, the move mix and the ALNS knobs. Shared by both solve()
// overloads.
optimaru::SolverConfig buildEngineConfig(const Solver::Config &config,
                                         const FlightCrewSchedule &schedule,
                                         int multiStartCount)
{
    optimaru::SolverConfig engineConfig;
    engineConfig.randomSeed = config.randomSeed;
    engineConfig.environmentMode = config.environmentMode;
    engineConfig.multiStartCount = multiStartCount;
    engineConfig.onNewBest = config.onNewBest;
    engineConfig.termination.spentLimit = config.spentLimit;
    engineConfig.termination.bestScoreLimit = config.bestScoreLimit;
    engineConfig.termination.unimprovedSpentLimit = config.unimprovedSpentLimit;
    engineConfig.termination.stepCountLimit = config.stepCountLimit;
    engineConfig.termination.unimprovedStepCountLimit
        = config.unimprovedStepCountLimit;
    // lateAcceptanceSize 0 = auto-scale with the instance (see Config).
    // Coefficient 4 measured against 8 and 2 (10s, 6 seeds, single thread):
    // the score is hard-lexicographic with a rugged soft landscape, so a long
    // acceptance history keeps re-accepting soft regressions it cannot pay
    // back — 4 wins by 15% soft on the 4x instance and 4% on 2x, with 2 (and
    // every larger coefficient) losing on 2x.
    engineConfig.localSearch.acceptor.lateAcceptanceSize
        = config.lateAcceptanceSize != 0
              ? config.lateAcceptanceSize
              : std::clamp<std::size_t>(
                    4 * schedule.flightAssignments.size(), 400, 3200);
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

    // TEMPORARY SCREENING SCAFFOLDING (tuning session): env-var overrides so
    // one build can sweep candidate knobs. Removed before the session ends.
    if (const char *dw = std::getenv("FCS_DESCENT_WEIGHT"))
        engineConfig.localSearch.descentWeight = std::atof(dw);
    if (const char *aw = std::getenv("FCS_ASMOVE_WEIGHT"))
        lns.asMoveWeight = std::atof(aw);
    if (const char *ar = std::getenv("FCS_ASMOVE_MAXRUIN"))
        lns.asMoveMaxRuin = std::atoi(ar);

    return engineConfig;
}

}  // namespace

void Solver::solve(FlightCrewSchedule &schedule)
{
    ScoreCalculator::prepare(schedule);
    if (schedule.flightAssignments.empty() || schedule.employees.empty())
    {
        ScoreCalculator::calculateScore(schedule);
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }

    auto engine
        = optimaru::SolverFactory<FlightCrewSchedule>::create(
              buildEngineConfig(
                  config_, schedule, std::max(1, config_.parallelSolverCount)))
              .buildSolver();
    engine.solveMultiStart(
        schedule,
        []
        { return std::make_unique<FlightCrewIncrementalScoreCalculator>(); });

    // The authoritative final score: a full recompute on the best solution.
    schedule.score = ScoreCalculator::calculateScore(schedule);
}

void Solver::solve(
    FlightCrewSchedule &schedule,
    optimaru::DecomposedScoreCalculator<FlightCrewSchedule> &calculator)
{
    ScoreCalculator::prepare(schedule);
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }
    if (!schedule.flightAssignments.empty() && !schedule.employees.empty())
    {
        // Single-start: the caller-owned calculator cannot be cloned, so the
        // multi-start factory would have nothing to hand its extra workers.
        auto engine = optimaru::SolverFactory<FlightCrewSchedule>::create(
                          buildEngineConfig(config_, schedule, 1))
                          .buildSolver();
        engine.solve(schedule, calculator);
    }

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraintTotals() explains exactly this score.
    calculator.resetWorkingSolution(schedule);
    schedule.score = calculator.calculateScore();
}

}  // namespace fcs

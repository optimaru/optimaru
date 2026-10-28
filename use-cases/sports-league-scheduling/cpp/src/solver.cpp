#include "sls/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "sls/incremental_calculator.hpp"
#include "sls/score_calculator.hpp"
#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"

namespace sls
{

namespace
{

// Fills the engine config from the domain config. The nearby distance meter
// captures `schedule` by pointer, so this must only ever be used on a
// single-start solve of exactly that schedule.
void configureEngine(const Solver::Config &config,
                     LeagueSchedule &schedule,
                     optimaru::SolverConfig &engineConfig)
{
    engineConfig.randomSeed = config.randomSeed;
    engineConfig.environmentMode = config.environmentMode;
    engineConfig.multiStartCount = 1;
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
                    32 * schedule.matches.size(), 400, 3200);
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

    // Nearby selection (see Config::nearbySize): two matches are "near" when
    // they share a team — the only way two matches can directly trade travel
    // — and among those the closest are the ones whose venues are closest.
    // Biases the swap partner and drives the LNS related ruin toward one
    // team's cluster of fixtures.
    if (config.nearbySize >= 0)
    {
        const LeagueSchedule *s = &schedule;
        engineConfig.localSearch.nearby.distanceMeter = [s](int a, int b)
        {
            const Match &x = s->matches[a];
            const Match &y = s->matches[b];
            const bool shared
                = x.homeTeam == y.homeTeam || x.homeTeam == y.awayTeam
                  || x.awayTeam == y.homeTeam || x.awayTeam == y.awayTeam;
            const double venueGap
                = static_cast<double>(s->distance(x.homeTeam, y.homeTeam)
                                      + s->distance(x.awayTeam, y.awayTeam));
            return (shared ? 0.0 : 1.0e9) + venueGap;
        };
        // nearbySize 0 = auto (see Config): a match's whole shared-team set.
        engineConfig.localSearch.nearby.size = config.nearbySize > 0
                                                   ? config.nearbySize
                                                   : 4 * schedule.teamCount();
        engineConfig.localSearch.nearby.probability = config.nearbyProbability;
    }
}

}  // namespace

void Solver::solve(LeagueSchedule &schedule)
{
    ScoreCalculator::prepare(schedule);  // intern pair ids / index tables
    if (schedule.matches.empty() || schedule.rounds.empty())
    {
        ScoreCalculator::calculateScore(schedule);
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }

    // Independent parallel starts (the school-timetabling pattern): worker i
    // solves its own copy with seed + i. The guidance hooks below capture the
    // working solution and the live calculator, so the engine's built-in
    // multi-start — which shares one config across threads — cannot be used.
    // The best final score wins.
    const int workerCount = std::max(1, config_.parallelSolverCount);
    if (workerCount > 1)
    {
        std::vector<LeagueSchedule> candidates(workerCount, schedule);
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

    optimaru::SolverConfig engineConfig;
    configureEngine(config_, schedule, engineConfig);

    // The calculator of this (single-start) solve, created before the engine
    // so the guidance hook below can bind to it; the engine resets it on the
    // working solution at solve start.
    SportsLeagueIncrementalScoreCalculator calculator;

    // Travel-guided entity selection (the "worst" removal of ALNS, applied to
    // move selection): the moved match is the most expensive of
    // costliestMatchSample randomly drawn matches. Hard violations are gone
    // within milliseconds here — the whole 30s budget goes into travel — so
    // the guidance that pays off is toward the matches actually carrying the
    // travel, not toward violations.
    if (config_.costliestMatchSample > 1)
    {
        const int sample = config_.costliestMatchSample;
        engineConfig.localSearch.entityRecommender
            = [&calculator, sample](std::mt19937_64 &rng) -> int
        { return calculator.costliestMatch(rng, sample); };
    }

    // Construction heuristic + late-acceptance local search (+ ALNS kicks).
    optimaru::Solver<LeagueSchedule> engine(std::move(engineConfig));
    engine.solve(schedule, calculator);

    // The authoritative final score: a full recompute on the best solution.
    schedule.score = ScoreCalculator::calculateScore(schedule);
}

void Solver::solve(
    LeagueSchedule &schedule,
    optimaru::DecomposedScoreCalculator<LeagueSchedule> &calculator)
{
    ScoreCalculator::prepare(schedule);  // intern pair ids / index tables
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }
    if (!schedule.matches.empty() && schedule.roundCount() != 0)
    {
        // No travel-guided entity recommender here: it reads the monolithic
        // calculator's per-match travel costs, which a caller-assembled
        // plugin does not expose. The built-in move mix plus the ALNS kicks
        // remain.
        optimaru::SolverConfig engineConfig;
        configureEngine(config_, schedule, engineConfig);
        optimaru::Solver<LeagueSchedule> engine(std::move(engineConfig));
        engine.solve(schedule, calculator);
    }

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraintTotals() explains exactly this score.
    calculator.resetWorkingSolution(schedule);
    schedule.score = calculator.calculateScore();
}

}  // namespace sls

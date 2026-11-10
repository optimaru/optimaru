#include "sls/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "sls/incremental_calculator.hpp"
#include "sls/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/worker_threads.hpp"

namespace sls {

namespace {

// Fills the engine config from the domain config. The nearby distance meter
// captures `schedule` by pointer, so this must only ever be used on a
// single-start solve of exactly that schedule.
void configureEngine(const Solver::Config& config, LeagueSchedule& schedule,
                     optimaru::Settings& engineConfig) {
    engineConfig.seed = config.seed;
    engineConfig.score_checks = config.score_checks;
    engineConfig.parallel_starts = 1;
    engineConfig.on_new_best = config.on_new_best;
    engineConfig.limits.time_limit = config.time_limit;
    engineConfig.limits.target_score = config.target_score;
    engineConfig.limits.stall_time_limit = config.stall_time_limit;
    engineConfig.limits.step_limit = config.step_limit;
    engineConfig.limits.stall_step_limit = config.stall_step_limit;
    // history_length 0 = auto-scale with the instance (see Config).
    engineConfig.search.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(32 * schedule.matches.size(), 400, 3200);
    engineConfig.search.moves.change_weight = config.change_weight;
    engineConfig.search.moves.swap_weight = config.swap_weight;
    auto& lns = engineConfig.search.ruin_recreate;
    lns.enabled = config.ruin_recreate;
    lns.unimproved_step_trigger = config.ruinRecreateUnimprovedStepTrigger;
    lns.min_ruin_fraction = config.ruinRecreateMinRuinFraction;
    lns.max_ruin_fraction = config.ruinRecreateMaxRuinFraction;
    lns.max_ruin_count = config.ruinRecreateMaxRuinCount;
    lns.operator_decay = config.ruinRecreateOperatorDecay;
    lns.rewards = config.ruinRecreateRewards;
    lns.polish_steps = config.ruinRecreatePolishSteps;

    // Nearby selection (see Config::nearbySize): two matches are "near" when
    // they share a team — the only way two matches can directly trade travel
    // — and among those the closest are the ones whose venues are closest.
    // Biases the swap partner and drives the LNS related ruin toward one
    // team's cluster of fixtures.
    if (config.nearbySize >= 0) {
        const LeagueSchedule* s = &schedule;
        engineConfig.search.nearby.distance = [s](int a, int b) {
            const Match& x = s->matches[a];
            const Match& y = s->matches[b];
            const bool shared = x.homeTeam == y.homeTeam || x.homeTeam == y.awayTeam ||
                                x.awayTeam == y.homeTeam || x.awayTeam == y.awayTeam;
            const double venueGap = static_cast<double>(s->distance(x.homeTeam, y.homeTeam) +
                                                        s->distance(x.awayTeam, y.awayTeam));
            return (shared ? 0.0 : 1.0e9) + venueGap;
        };
        // nearbySize 0 = auto (see Config): a match's whole shared-team set.
        engineConfig.search.nearby.size =
                config.nearbySize > 0 ? config.nearbySize : 4 * schedule.teamCount();
        engineConfig.search.nearby.probability = config.nearbyProbability;
    }
}

}  // namespace

void Solver::solve(LeagueSchedule& schedule) {
    ScoreCalculator::prepare(schedule);  // intern pair ids / index tables
    if (schedule.matches.empty() || schedule.rounds.empty()) {
        ScoreCalculator::score(schedule);
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4) {
        throw std::invalid_argument("ruinRecreateRewards must contain exactly 4 values");
    }

    // Independent parallel starts (the school-timetabling pattern): worker i
    // solves its own copy with seed + i. The guidance hooks below capture the
    // working solution and the live calculator, so the engine's built-in
    // multi-start — which shares one config across threads — cannot be used.
    // The best final score wins.
    const int workerCount = std::max(1, config_.parallelSolverCount);
    if (workerCount > 1) {
        std::vector<LeagueSchedule> candidates(workerCount, schedule);
        std::vector<std::exception_ptr> failures(workerCount);
        // Raised by the first worker that fails: every worker's engine checks
        // it once a step, so the others stop instead of using up their budget.
        auto stop = std::make_shared<std::atomic<bool>>(false);
        // A worker that cannot be started (std::system_error) stops and joins
        // the ones already running before the error is rethrown here.
        optimaru::run_worker_threads(workerCount, *stop,
                                     [this, &candidates, &failures, &stop](int i) {
                                         try {
                                             Config workerConfig = config_;
                                             workerConfig.parallelSolverCount = 1;
                                             workerConfig.stop_flag = stop;
                                             workerConfig.seed += static_cast<std::uint64_t>(i);
                                             if (i != 0) workerConfig.on_new_best = nullptr;
                                             Solver worker(std::move(workerConfig));
                                             worker.solve(candidates[i]);
                                         } catch (...) {
                                             failures[i] = std::current_exception();
                                             stop->store(true);
                                         }
                                     });
        for (const std::exception_ptr& failure : failures) {
            if (failure) std::rethrow_exception(failure);
        }
        int best = 0;
        for (int i = 1; i < workerCount; ++i) {
            if (candidates[i].score > candidates[best].score) best = i;
        }
        schedule = std::move(candidates[best]);
        return;
    }

    optimaru::Settings engineConfig;
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
    if (config_.costliestMatchSample > 1) {
        const int sample = config_.costliestMatchSample;
        engineConfig.search.entity_hint = [&calculator, sample](std::mt19937_64& rng) -> int {
            return calculator.costliestMatch(rng, sample);
        };
    }

    // Construction heuristic + late-acceptance local search (+ ALNS kicks).
    optimaru::Solver<LeagueSchedule> engine(std::move(engineConfig));
    if (config_.stop_flag) engine.share_stop_flag(config_.stop_flag);
    engine.solve(schedule, calculator);

    // The authoritative final score: a full recompute on the best solution.
    schedule.score = ScoreCalculator::score(schedule);
}

void Solver::solve(LeagueSchedule& schedule,
                   optimaru::DecomposedObjective<LeagueSchedule>& calculator) {
    ScoreCalculator::prepare(schedule);  // intern pair ids / index tables
    if (config_.ruinRecreateRewards.size() != 4) {
        throw std::invalid_argument("ruinRecreateRewards must contain exactly 4 values");
    }
    if (!schedule.matches.empty() && schedule.roundCount() != 0) {
        // No travel-guided entity recommender here: it reads the monolithic
        // calculator's per-match travel costs, which a caller-assembled
        // plugin does not expose. The built-in move mix plus the ALNS kicks
        // remain.
        optimaru::Settings engineConfig;
        configureEngine(config_, schedule, engineConfig);
        optimaru::Solver<LeagueSchedule> engine(std::move(engineConfig));
        if (config_.stop_flag) engine.share_stop_flag(config_.stop_flag);
        engine.solve(schedule, calculator);
    }

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraint_totals() explains exactly this score.
    calculator.rebuild(schedule);
    schedule.score = calculator.score();
}

}  // namespace sls

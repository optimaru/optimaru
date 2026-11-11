#include "sls/solver.hpp"

#include <algorithm>
#include <random>
#include <utility>

#include "sls/incremental_calculator.hpp"
#include "sls/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"

namespace sls {

namespace {

// Fills the engine config from the domain config. The nearby distance meter
// captures `schedule` by pointer, so this must only ever be used on a
// single-start solve of exactly that schedule.
void configureEngine(const Solver::Config& config, LeagueSchedule& schedule,
                     optimaru::Settings& engineConfig) {
    engineConfig = optimaru::settings_from(config);
    engineConfig.parallel_starts = 1;
    // history_length 0 = auto-scale with the instance (see Config).
    engineConfig.search.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(32 * schedule.matches.size(), 400, 3200);
    // Restart late acceptance with a doubled history when the search stalls
    // (see AcceptanceSettings::history_growth): measured on every instance of
    // benchmarks/sports-league-scheduling, none worse at 30 s on three seeds.
    engineConfig.search.acceptance.history_growth = 2.0;
    engineConfig.search.moves.change_weight = config.change_weight;
    engineConfig.search.moves.swap_weight = config.swap_weight;

    // Nearby selection (see Config::nearby_size): two matches are "near" when
    // they share a team — the only way two matches can directly trade travel
    // — and among those the closest are the ones whose venues are closest.
    // Biases the swap partner and drives the LNS related ruin toward one
    // team's cluster of fixtures.
    if (config.nearby_size >= 0) {
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
        // nearby_size 0 = auto (see Config): a match's whole shared-team set.
        engineConfig.search.nearby.size =
                config.nearby_size > 0 ? config.nearby_size : 4 * schedule.teamCount();
        engineConfig.search.nearby.probability = config.nearby_probability;
    }
}

}  // namespace

void Solver::solve(LeagueSchedule& schedule) {
    ScoreCalculator::prepare(schedule);  // intern pair ids / index tables
    if (schedule.matches.empty() || schedule.rounds.empty()) {
        ScoreCalculator::score(schedule);
        return;
    }
    optimaru::check_config(config_);

    // Independent parallel starts (the school-timetabling pattern): each start
    // is a single-start Solver of its own. The guidance hooks below capture the
    // working solution and the live calculator, so the engine's built-in
    // multi-start — which shares one config across threads — cannot be used.
    if (config_.parallel_solver_count > 1) {
        optimaru::solve_parallel_starts(config_, schedule,
                                        [](const Config& config, LeagueSchedule& candidate, int) {
                                            Solver worker(config);
                                            worker.solve(candidate);
                                        });
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
    // costliest_match_sample randomly drawn matches. Hard violations are gone
    // within milliseconds here — the whole 30s budget goes into travel — so
    // the guidance that pays off is toward the matches actually carrying the
    // travel, not toward violations.
    if (config_.costliest_match_sample > 1) {
        const int sample = config_.costliest_match_sample;
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
    optimaru::check_config(config_);
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

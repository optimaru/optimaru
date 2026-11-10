#include "fcs/solver.hpp"

#include <algorithm>
#include <memory>

#include "fcs/incremental_calculator.hpp"
#include "fcs/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"

namespace fcs {

namespace {

// Translates a domain Config into the engine's Settings: termination,
// late acceptance, the move mix and the ALNS knobs. Shared by both solve()
// overloads.
optimaru::Settings buildEngineConfig(const Solver::Config& config,
                                     const FlightCrewSchedule& schedule, int parallel_starts) {
    optimaru::Settings engineConfig = optimaru::settings_from(config);
    engineConfig.parallel_starts = parallel_starts;
    // history_length 0 = auto-scale with the instance (see Config).
    // Coefficient 4 measured against 8 and 2 (10s, 6 seeds, single thread):
    // the score is hard-lexicographic with a rugged soft landscape, so a long
    // acceptance history keeps re-accepting soft regressions it cannot pay
    // back — 4 wins by 15% soft on the 4x instance and 4% on 2x, with 2 (and
    // every larger coefficient) losing on 2x.
    engineConfig.search.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(4 * schedule.flightAssignments.size(), 400, 3200);
    engineConfig.search.moves.change_weight = config.change_weight;
    engineConfig.search.moves.swap_weight = config.swap_weight;

    return engineConfig;
}

}  // namespace

void Solver::solve(FlightCrewSchedule& schedule) {
    ScoreCalculator::prepare(schedule);
    if (schedule.flightAssignments.empty() || schedule.employees.empty()) {
        ScoreCalculator::score(schedule);
        return;
    }
    optimaru::check_config(config_);

    optimaru::Solver<FlightCrewSchedule> engine(
            buildEngineConfig(config_, schedule, std::max(1, config_.parallel_solver_count)));
    engine.solve_multistart(
            schedule, [] { return std::make_unique<FlightCrewIncrementalScoreCalculator>(); });

    // The authoritative final score: a full recompute on the best solution.
    schedule.score = ScoreCalculator::score(schedule);
}

void Solver::solve(FlightCrewSchedule& schedule,
                   optimaru::DecomposedObjective<FlightCrewSchedule>& calculator) {
    ScoreCalculator::prepare(schedule);
    optimaru::check_config(config_);
    if (!schedule.flightAssignments.empty() && !schedule.employees.empty()) {
        // Single-start: the caller-owned calculator cannot be cloned, so the
        // multi-start factory would have nothing to hand its extra workers.
        auto engine = optimaru::Solver<FlightCrewSchedule>(buildEngineConfig(config_, schedule, 1));
        engine.solve(schedule, calculator);
    }

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraint_totals() explains exactly this score.
    calculator.rebuild(schedule);
    schedule.score = calculator.score();
}

}  // namespace fcs

#include "fab/solver.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "fab/decoder.hpp"
#include "fab/incremental_calculator.hpp"
#include "fab/moves/priority_swap.hpp"
#include "fab/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/accept/acceptors.hpp"

namespace fab {

void beginSearch(SemiconductorSchedule& schedule) {
    ScoreCalculator::prepare(schedule);
    const int n = schedule.rowCount();
    const std::vector<int> choices = defaultChoices(schedule);
    std::vector<int> selected;
    std::vector<int> ranks;
    if (schedule.warmStart) {
        seedState(schedule, choices, selected, ranks);
    } else {
        selected = choices;
        ranks.resize(static_cast<std::size_t>(n));
        std::iota(ranks.begin(), ranks.end(), 0);
    }
    for (int r = 0; r < n; ++r) {
        schedule.rows[static_cast<std::size_t>(r)].value = schedule.valueOf(
                ranks[static_cast<std::size_t>(r)], selected[static_cast<std::size_t>(r)]);
    }
    // A complete seed (every operation assigned; the list is validated
    // unique) stands for itself while the values stay at the seed state.
    schedule.seedComplete =
            schedule.warmStart && static_cast<int>(schedule.assignments.size()) == n;
    schedule.seedValues.clear();
    schedule.seedAssignments.clear();
    if (schedule.seedComplete) {
        for (const OperationRow& row : schedule.rows) schedule.seedValues.push_back(row.value);
        schedule.seedAssignments = schedule.assignments;
    }
    schedule.searching = true;
}

void endSearch(SemiconductorSchedule& schedule) {
    bool seed = schedule.seedComplete && schedule.seedValues.size() == schedule.rows.size();
    for (std::size_t r = 0; seed && r < schedule.rows.size(); ++r)
        seed = schedule.rows[r].value == schedule.seedValues[r];
    schedule.assignments = seed ? schedule.seedAssignments : decodeValues(schedule);
    schedule.searching = false;
    schedule.warmStart = false;  // a seed is used once
    schedule.seedComplete = false;
    schedule.seedValues.clear();
    schedule.seedAssignments.clear();
    ScoreCalculator::score(schedule);
}

namespace {

// Leaves search mode without touching the assignment list, for a solve that
// failed after beginSearch() (for example the decoder giving up). Otherwise
// the schedule would stay in search mode, and the plugin path would re-decode
// the decision values while the monolithic calculator scores `assignments`.
void abandonSearch(SemiconductorSchedule& schedule) {
    schedule.searching = false;
    schedule.seedComplete = false;
    schedule.seedValues.clear();
    schedule.seedAssignments.clear();
}

void validate(const Solver::Config& config) {
    optimaru::check_config(config);
    if (config.parallel_solver_count < 1)
        throw std::invalid_argument("parallel_solver_count must be positive");
    if (config.machine_move_probability < 0.0 || config.machine_move_probability > 1.0)
        throw std::invalid_argument("machine_move_probability must be in [0, 1]");
}

// The change move's value: another machine for the row (rank kept), or a
// small priority shift (machine kept). -1 = the engine's uniform draw.
std::function<int(std::mt19937_64&, int)> value_hint(const SemiconductorSchedule* working,
                                                     double machine_move_probability) {
    return [working, machine_move_probability](std::mt19937_64& rng, int row) -> int {
        const SemiconductorSchedule& s = *working;
        const int value = s.rows[static_cast<std::size_t>(row)].value;
        if (value < 0) return -1;
        const int rank = s.rankOf(value);
        const int slot = s.slotOf(value);
        const int options = s.optionCountOfRow(row);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        if (options > 1 && unit(rng) < machine_move_probability) {
            const int shift = 1 + static_cast<int>(rng() % static_cast<std::uint64_t>(options - 1));
            return s.valueOf(rank, (slot % options + shift) % options);
        }
        const int n = s.rowCount();
        if (n < 2) return -1;
        int magnitude = 1;  // geometric 1..8
        while ((rng() & 1u) != 0 && magnitude < 8) ++magnitude;
        const int sign = (rng() & 1u) != 0 ? 1 : -1;
        int shifted = std::clamp(rank + sign * magnitude, 0, n - 1);
        if (shifted == rank) shifted = std::clamp(rank - sign * magnitude, 0, n - 1);
        return shifted == rank ? -1 : s.valueOf(shifted, slot);
    };
}

}  // namespace

void Solver::runSingle(SemiconductorSchedule& schedule,
                       optimaru::Objective<SemiconductorSchedule>& calculator) const {
    optimaru::Settings engineConfig = optimaru::settings_from(config_);

    engineConfig.search.acceptance.history_length =
            config_.history_length != 0
                    ? config_.history_length
                    : std::clamp<std::size_t>(16 * schedule.rows.size(), 400, 3200);

    engineConfig.search.moves.change_weight = config_.change_weight;
    engineConfig.search.moves.swap_weight = config_.swap_weight;
    engineConfig.search.custom_move_probability = config_.custom_move_probability;
    // The schedule outlives the engine run: the recommender reads the
    // working values through it.
    engineConfig.search.value_hint = value_hint(&schedule, config_.machine_move_probability);

    optimaru::Solver<SemiconductorSchedule> engine(std::move(engineConfig));
    if (config_.stop_flag) engine.share_stop_flag(config_.stop_flag);
    if (config_.priority_swap_moves) engine.add_move_operator(std::make_unique<PrioritySwapMove>());
    engine.solve(schedule, calculator);
}

void Solver::solve(SemiconductorSchedule& schedule) {
    validate(config_);
    beginSearch(schedule);
    try {
        searchFromBegun(schedule);
    } catch (...) {
        abandonSearch(schedule);
        throw;
    }
}

void Solver::searchFromBegun(SemiconductorSchedule& schedule) {
    if (schedule.rows.empty()) {
        endSearch(schedule);
        return;
    }
    if (config_.parallel_solver_count > 1) {
        // Domain-level starts: the value recommender and the priority swap
        // read the working schedule, so every start needs its own copy. The
        // starts are compared by their search-state score, which each leaves
        // in candidate.score; endSearch() below stores the data-mode score.
        optimaru::solve_parallel_starts(
                config_, schedule, [](const Config& config, SemiconductorSchedule& candidate, int) {
                    SemiconductorIncrementalScoreCalculator calculator;
                    Solver(config).runSingle(candidate, calculator);
                    candidate.score = ScoreCalculator::searchStateScore(candidate);
                });
    } else {
        SemiconductorIncrementalScoreCalculator calculator;
        runSingle(schedule, calculator);
    }
    endSearch(schedule);
}

void Solver::solve(SemiconductorSchedule& schedule,
                   optimaru::DecomposedObjective<SemiconductorSchedule>& calculator) {
    validate(config_);
    beginSearch(schedule);
    try {
        if (!schedule.rows.empty()) runSingle(schedule, calculator);
        endSearch(schedule);  // assignments + the default totals
    } catch (...) {
        abandonSearch(schedule);
        throw;
    }
    // Score with the caller's assembly, in data mode, and leave the
    // calculator primed on the result.
    calculator.rebuild(schedule);
    schedule.score = calculator.score();
}

}  // namespace fab

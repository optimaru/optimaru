#include "st/solver.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "st/incremental_calculator.hpp"
#include "st/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"

namespace st {

void Solver::solve(Timetable& table) {
    ScoreCalculator::prepare(table);  // intern ids / build slot adjacency
    if (table.lessons.empty() || table.slotRoomCount() == 0) {
        ScoreCalculator::score(table);
        return;
    }
    if (config_.ruin_recreate_rewards.size() != 4) {
        throw std::invalid_argument("ruin_recreate_rewards must contain exactly 4 values");
    }

    optimaru::Settings engineConfig;
    engineConfig.seed = config_.seed;
    engineConfig.score_checks = config_.score_checks;
    engineConfig.parallel_starts = std::max(1, config_.parallel_solver_count);
    engineConfig.on_new_best = config_.on_new_best;
    engineConfig.limits.time_limit = config_.time_limit;
    engineConfig.limits.target_score = config_.target_score;
    engineConfig.limits.stall_time_limit = config_.stall_time_limit;
    engineConfig.limits.step_limit = config_.step_limit;
    engineConfig.limits.stall_step_limit = config_.stall_step_limit;
    // history_length 0 = auto-scale with the instance (see Config).
    engineConfig.search.acceptance.history_length =
            config_.history_length != 0
                    ? config_.history_length
                    : std::clamp<std::size_t>(8 * table.lessons.size(), 400, 3200);
    engineConfig.search.moves.change_weight = config_.change_weight;
    engineConfig.search.moves.swap_weight = config_.swap_weight;
    auto& lns = engineConfig.search.ruin_recreate;
    lns.enabled = config_.ruin_recreate;
    lns.unimproved_step_trigger = config_.ruin_recreate_unimproved_step_trigger;
    lns.min_ruin_fraction = config_.ruin_recreate_min_ruin_fraction;
    lns.max_ruin_fraction = config_.ruin_recreate_max_ruin_fraction;
    lns.max_ruin_count = config_.ruin_recreate_max_ruin_count;
    lns.operator_decay = config_.ruin_recreate_operator_decay;
    lns.rewards = config_.ruin_recreate_rewards;
    lns.polish_steps = config_.ruin_recreate_polish_steps;
    // AUTO as-move weight (negative sentinel): budget-share 0.2 inside the
    // measured [900, 1300]-cell pay-off window, off outside it (see Config).
    if (config_.ruin_recreate_as_move_weight < 0.0) {
        const int cells = table.slotRoomCount();
        lns.as_move_weight = (cells >= 900 && cells <= 1300) ? 0.2 / cells : 0.0;
    } else {
        lns.as_move_weight = config_.ruin_recreate_as_move_weight;
    }
    lns.as_move_max_ruin = config_.ruin_recreate_as_move_max_ruin;
    engineConfig.search.descent_weight = config_.descent_weight;
    engineConfig.search.descent_max_sweeps = config_.descent_max_sweeps;

    // Same-group nearby neighborhoods (see Config::same_group_nearby): the
    // groupId is interned by prepare().
    if (config_.same_group_nearby) {
        auto groupOf = std::make_shared<std::vector<int>>();
        groupOf->reserve(table.lessons.size());
        std::vector<std::size_t> groupSize(std::max(1, table.groupCount), 0);
        for (const Lesson& lesson : table.lessons) {
            groupOf->push_back(lesson.groupId);
            ++groupSize[lesson.groupId];
        }
        const std::size_t maxGroupSize = *std::max_element(groupSize.begin(), groupSize.end());
        engineConfig.search.nearby.distance = [groupOf](int a, int b) {
            return (*groupOf)[a] == (*groupOf)[b] ? 0.0 : 1.0;
        };
        engineConfig.search.nearby.size = std::max(1, static_cast<int>(maxGroupSize) - 1);
        engineConfig.search.nearby.probability = config_.same_group_nearby_probability;
    }

    // Construction heuristic + late-acceptance local search (+ ALNS kicks),
    // multi-start parallel when parallel_solver_count > 1. Each start gets its
    // own calculator instance (the calculators are stateful); the shared
    // nearby meter above is a pure read of the classOf table, so it is safe
    // across the multi-start threads.
    auto engine = optimaru::Solver<Timetable>(std::move(engineConfig));
    engine.solve_multistart(
            table, [] { return std::make_unique<SchoolTimetablingIncrementalScoreCalculator>(); });

    // The authoritative final score: a full recompute on the best solution.
    table.score = ScoreCalculator::score(table);
}

}  // namespace st

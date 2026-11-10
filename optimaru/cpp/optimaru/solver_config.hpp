// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SOLVER_CONFIG_HPP
#define OPTIMARU_SOLVER_CONFIG_HPP

// What the Solver::Config of every use case shares, and the three things every
// use case's solver does with it: check it, turn it into the engine's
// Settings, and run independent parallel starts of its own single-start solve.
//
//   struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
//       double change_weight = 0.75;  // ... the use case's own knobs
//   };
//
//   optimaru::check_config(config);                           // before anything is touched
//   optimaru::Settings settings = optimaru::settings_from(config);
//   settings.search.moves.change_weight = config.change_weight;  // ... its own knobs
//
// optimaru/python/solver_config.hpp binds the same fields to Python, under the
// same names.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "optimaru/score/score.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/worker_threads.hpp"

namespace optimaru {

// The first base of a use case's Solver::Config. ScoreT is the Score the use
// case reports: the engine's (hard, soft) Score, or a three-level one of its
// own that declares `static ScoreT unreachable()`.
template <typename ScoreT>
struct SolverConfigBase {
    using score_type = ScoreT;

    // Wall-clock budget. 0 stops before the first step: the solve then returns
    // the construction (or a complete seed) unchanged.
    std::chrono::milliseconds time_limit{std::chrono::seconds(30)};

    // Stop early once the best score reaches this. The default is above every
    // score, so it never fires.
    ScoreT target_score = ScoreT::unreachable();
    // Optional early exits; 0 disables each one (so step_limit = 0 means "no
    // step limit", not "no search").
    std::chrono::milliseconds stall_time_limit{0};
    std::int64_t step_limit = 0;
    std::int64_t stall_step_limit = 0;

    // Late-acceptance history length. 0 = the use case's automatic size.
    std::size_t history_length = 0;
    std::uint64_t seed = 0;

    // Independent parallel starts: start i searches its own copy of the
    // constructed problem with seed + i and the full budget; the best score
    // wins (the lowest start on ties).
    int parallel_solver_count = 1;

    // ScoreChecks::Full re-verifies the incremental score against a full
    // recompute after every move (a debugging aid; much slower).
    ScoreChecks score_checks = ScoreChecks::Off;

    // Called on every new best as on_new_best(step, score). With parallel
    // starts only start 0 reports.
    std::function<void(std::int64_t, const ScoreT&)> on_new_best;

    // The run-scoped stop of solve_parallel_starts: the first start that fails
    // raises it, and every start's engine checks it once a step. Null outside
    // a parallel run.
    std::shared_ptr<std::atomic<bool>> stop_flag;
};

// The second base, for a use case that runs the engine's adaptive ruin &
// recreate (ALNS) kicks: the knobs of RuinRecreateSettings, with its defaults.
// A use case tuned differently sets its own in its Config's constructor.
struct RuinRecreateConfig {
    bool ruin_recreate = true;
    std::int64_t ruin_recreate_unimproved_step_trigger = 5000;
    double ruin_recreate_min_ruin_fraction = 0.10;
    double ruin_recreate_max_ruin_fraction = 0.30;
    int ruin_recreate_max_ruin_count = 60;
    double ruin_recreate_operator_decay = 0.8;
    // Four rewards, indexed NEW_BEST / BETTER / ACCEPTED / REJECTED.
    std::vector<double> ruin_recreate_rewards{33.0, 9.0, 3.0, 0.0};
    std::int64_t ruin_recreate_polish_steps = 5000;
};

namespace detail {

// Whether Config derives from SolverConfigBase<its score type>.
template <typename Config, typename = void>
struct HasSolverConfigBase : std::false_type {};
template <typename Config>
struct HasSolverConfigBase<Config, std::void_t<typename Config::score_type>>
    : std::is_base_of<SolverConfigBase<typename Config::score_type>, Config> {};

template <typename Config, typename = void>
struct HasAsMoveKnobs : std::false_type {};
template <typename Config>
struct HasAsMoveKnobs<
        Config, std::void_t<decltype(std::declval<const Config&>().ruin_recreate_as_move_weight)>>
    : std::true_type {};

}  // namespace detail

// What cannot be checked field by field. Call it first, so a bad Config is
// refused before the problem is touched.
template <typename Config>
void check_config(const Config& config) {
    if constexpr (std::is_base_of_v<RuinRecreateConfig, Config>) {
        if (config.ruin_recreate_rewards.size() != 4) {
            throw std::invalid_argument("ruin_recreate_rewards must contain exactly 4 values");
        }
    } else {
        (void)config;
    }
}

// The engine Settings a use case starts from: every field of the shared bases
// in its place. The use case then sets what is its own (the acceptance history
// behind history_length = 0, the move mix, nearby selection, ...).
//
// A use case with a Score of its own also sets limits.target_score and
// on_new_best itself: both need its conversion to and from the engine's Score.
template <typename Config>
Settings settings_from(const Config& config) {
    Settings settings;
    settings.seed = config.seed;
    settings.score_checks = config.score_checks;
    settings.limits.time_limit = config.time_limit;
    settings.limits.stall_time_limit = config.stall_time_limit;
    settings.limits.step_limit = config.step_limit;
    settings.limits.stall_step_limit = config.stall_step_limit;
    if constexpr (std::is_base_of_v<SolverConfigBase<Score>, Config>) {
        settings.limits.target_score = config.target_score;
        settings.on_new_best = config.on_new_best;
    }
    if constexpr (std::is_base_of_v<RuinRecreateConfig, Config>) {
        RuinRecreateSettings& kicks = settings.search.ruin_recreate;
        kicks.enabled = config.ruin_recreate;
        kicks.unimproved_step_trigger = config.ruin_recreate_unimproved_step_trigger;
        kicks.min_ruin_fraction = config.ruin_recreate_min_ruin_fraction;
        kicks.max_ruin_fraction = config.ruin_recreate_max_ruin_fraction;
        kicks.max_ruin_count = config.ruin_recreate_max_ruin_count;
        kicks.operator_decay = config.ruin_recreate_operator_decay;
        kicks.rewards = config.ruin_recreate_rewards;
        kicks.polish_steps = config.ruin_recreate_polish_steps;
        if constexpr (detail::HasAsMoveKnobs<Config>::value) {
            kicks.as_move_weight = config.ruin_recreate_as_move_weight;
            kicks.as_move_max_ruin = config.ruin_recreate_as_move_max_ruin;
        }
    }
    return settings;
}

// Independent parallel starts of a use case's own single-start solve, for a
// solver the engine's Solver::solve_multistart cannot serve: one that registers
// custom operators, or whose Settings capture the problem they search.
//
// Start i runs solve_one(start_config, candidate, i) on a copy of `problem`,
// where start_config is `config` with seed + i, one start, the run's stop flag
// and (for i > 0) no on_new_best. solve_one leaves candidate.score set. The
// best score wins, the lowest start on ties. With one start, solve_one runs on
// `problem` itself with `config` as it is.
//
// Exception-safe like solve_multistart: a start that throws stops the others at
// their next step, every thread is joined, and the first failure (in start
// order) is rethrown here with `problem` left as it was.
template <typename Config, typename Problem, typename SolveOne>
void solve_parallel_starts(const Config& config, Problem& problem, const SolveOne& solve_one) {
    const int starts = std::max(1, config.parallel_solver_count);
    if (starts == 1) {
        solve_one(config, problem, 0);
        return;
    }
    std::vector<Problem> candidates(static_cast<std::size_t>(starts), problem);
    std::vector<std::exception_ptr> failures(candidates.size());
    auto stop = std::make_shared<std::atomic<bool>>(false);
    run_worker_threads(starts, *stop, [&config, &candidates, &failures, &stop, &solve_one](int i) {
        const auto index = static_cast<std::size_t>(i);
        try {
            Config start = config;
            start.parallel_solver_count = 1;
            start.stop_flag = stop;
            start.seed += static_cast<std::uint64_t>(i);
            if (i != 0) start.on_new_best = nullptr;  // only start 0 reports
            solve_one(start, candidates[index], i);
        } catch (...) {
            failures[index] = std::current_exception();
            stop->store(true);
        }
    });
    for (const std::exception_ptr& failure : failures) {
        if (failure) std::rethrow_exception(failure);
    }
    std::size_t best = 0;
    for (std::size_t i = 1; i < candidates.size(); ++i) {
        if (candidates[i].score > candidates[best].score) best = i;
    }
    problem = std::move(candidates[best]);
}

}  // namespace optimaru

#endif  // OPTIMARU_SOLVER_CONFIG_HPP

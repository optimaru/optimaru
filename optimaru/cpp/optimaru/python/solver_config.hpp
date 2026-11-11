// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_PYTHON_SOLVER_CONFIG_HPP
#define OPTIMARU_PYTHON_SOLVER_CONFIG_HPP

// Shared rules for every package's Python SolverConfig binding.
//
//   seconds_to_millis(s, "spent_limit_seconds")
//       A time limit in seconds (spent_limit_seconds,
//       unimproved_spent_limit_seconds, ...) must be finite and >= 0, and at
//       most kMaxTimeLimitSeconds (1e9 s, about 31.7 years), else
//       ValueError. NaN, inf and huge finite limits would take
//       duration_cast or the clock arithmetic out of range (undefined
//       behaviour; inf, 1e16 or sys.maxsize would read back as a negative or
//       zero budget, so the solve would stop after construction). 0 is a
//       valid limit (construction only).
//
//   bind_config_copy(py::class_<Config>(m, "SolverConfig", ...))
//       SolverConfig.__copy__ / __deepcopy__ (the C++ copy, on_new_best
//       included), so a package's solve(config=cfg, ...) can apply its
//       keyword overrides to a copy and leave the caller's config alone.

#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "optimaru/score/score.hpp"
#include "optimaru/solver_config.hpp"

namespace optimaru {
namespace python {

// The largest time limit a SolverConfig accepts. The engine compares a limit
// with steady_clock durations (nanoseconds on the usual platforms) and some
// packages add it to steady_clock::now() for a deadline, so the limit in
// nanoseconds plus the clock's own reading (time since boot) must fit an
// int64 (2^63 ns is about 292 years). 1e9 s leaves over 250 years of headroom
// and is still far beyond any real budget.
inline constexpr double kMaxTimeLimitSeconds = 1e9;

inline std::chrono::milliseconds seconds_to_millis(double seconds, const char* name) {
    const auto shown = [seconds] {
        return pybind11::repr(pybind11::float_(seconds)).cast<std::string>();
    };
    if (!std::isfinite(seconds) || seconds < 0.0) {
        throw pybind11::value_error(std::string(name) + " must be finite and >= 0, not " + shown());
    }
    if (seconds > kMaxTimeLimitSeconds) {
        throw pybind11::value_error(std::string(name) +
                                    " must be at most 1e9 seconds (about 31.7 years), not " +
                                    shown());
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::duration<double>(seconds));
}

template <typename Class>
Class& bind_config_copy(Class& cls) {
    using Config = typename Class::type;
    cls.def("__copy__", [](const Config& config) { return Config(config); });
    cls.def(
            "__deepcopy__",
            [](const Config& config, const pybind11::dict&) { return Config(config); },
            pybind11::arg("memo"));
    return cls;
}

template <typename Class>
Class bind_config_copy(Class&& cls) {
    bind_config_copy(cls);
    return std::move(cls);
}

namespace detail {

template <typename Config, typename = void>
struct HasRuinRecreateRewards : std::false_type {};
template <typename Config>
struct HasRuinRecreateRewards<Config,
                              std::void_t<decltype(std::declval<Config&>().ruin_recreate_rewards)>>
    : std::true_type {};

// A (hard, soft) amount an acceptance rule works with (a temperature, a water
// level step): both finite and >= 0.
inline Score step_score(const std::pair<std::int64_t, double>& value, const char* name) {
    if (value.first < 0 || !(value.second >= 0.0) || !std::isfinite(value.second)) {
        throw pybind11::value_error(std::string(name) + " must be finite and >= 0");
    }
    return Score{value.first, value.second};
}

}  // namespace detail

// `<pkg>._core.AcceptorType`: the engine's acceptance rules, for a use case
// whose SolverConfig is built on AcceptanceConfig (its `acceptor_type`). Call
// it before bind_solver_config.
inline void bind_acceptor_type(pybind11::module_& m) {
    pybind11::enum_<AcceptanceRule>(m, "AcceptorType", pybind11::module_local())
            .value("HILL_CLIMBING", AcceptanceRule::HillClimbing)
            .value("LATE_ACCEPTANCE", AcceptanceRule::LateAcceptance)
            .value("ENTITY_TABU", AcceptanceRule::EntityTabu)
            .value("SIMULATED_ANNEALING", AcceptanceRule::SimulatedAnnealing)
            .value("GREAT_DELUGE", AcceptanceRule::GreatDeluge)
            .value("STEP_COUNTING_HILL_CLIMBING", AcceptanceRule::StepCountingHillClimbing)
            .value("RECORD_TO_RECORD_TRAVEL", AcceptanceRule::RecordToRecordTravel);
}

// The part of SolverConfig that is the same in every use case, bound once so
// the names, the range checks and their messages cannot drift apart:
//
//   __copy__ / __deepcopy__          bind_config_copy
//   spent_limit_seconds              Config::time_limit         (seconds_to_millis)
//   unimproved_spent_limit_seconds   Config::stall_time_limit   (seconds_to_millis)
//   step_count_limit                 Config::step_limit         (0 = no limit)
//   unimproved_step_count_limit      Config::stall_step_limit   (0 = no limit)
//   random_seed                      Config::seed
//   parallel_solver_count            > 0
//   late_acceptance_size             Config::history_length, >= 0 with 0 = auto;
//                                    > 0 when `late_acceptance_auto` is false
//                                    (a use case without an automatic size)
//   full_assert                      Config::score_checks == ScoreChecks::Full
//   ruin_recreate_rewards            exactly 4 values (only where the Config has it)
//
// and, for a Config built on the bases of optimaru/solver_config.hpp:
//
//   on_new_best                      SolverConfigBase
//   best_score_limit                 SolverConfigBase<Score>::target_score as
//                                    (hard, soft); a use case with a Score of
//                                    its own binds it in that Score's shape
//   ruin_recreate, ruin_recreate_*   RuinRecreateConfig, and the two
//                                    ruin_recreate_as_move_* knobs where the
//                                    Config has them
//   acceptor_type and the rules'     AcceptanceConfig (bind_acceptor_type
//   parameters                       binds the enum, see there)
//
// A use case binds its own fields (move weights, nearby settings, ...) on
// the class this returns.
template <typename Class>
Class& bind_solver_config(Class& cls, bool late_acceptance_auto = true) {
    using Config = typename Class::type;
    bind_config_copy(cls);
    cls.def_property(
            "spent_limit_seconds",
            [](const Config& c) { return std::chrono::duration<double>(c.time_limit).count(); },
            [](Config& c, double s) {
                c.time_limit = seconds_to_millis(s, "spent_limit_seconds");
            });
    cls.def_property(
            "unimproved_spent_limit_seconds",
            [](const Config& c) {
                return std::chrono::duration<double>(c.stall_time_limit).count();
            },
            [](Config& c, double s) {
                c.stall_time_limit = seconds_to_millis(s, "unimproved_spent_limit_seconds");
            });
    cls.def_readwrite("step_count_limit", &Config::step_limit);
    cls.def_readwrite("unimproved_step_count_limit", &Config::stall_step_limit);
    cls.def_readwrite("random_seed", &Config::seed);
    cls.def_property(
            "parallel_solver_count", [](const Config& c) { return c.parallel_solver_count; },
            [](Config& c, int count) {
                if (count <= 0) {
                    throw pybind11::value_error("parallel_solver_count must be positive");
                }
                c.parallel_solver_count = count;
            });
    cls.def_property(
            "late_acceptance_size", [](const Config& c) { return c.history_length; },
            [late_acceptance_auto](Config& c, std::int64_t size) {
                if (late_acceptance_auto ? size < 0 : size <= 0) {
                    throw pybind11::value_error(
                            late_acceptance_auto ? "late_acceptance_size must be >= 0 (0 = auto)"
                                                 : "late_acceptance_size must be positive");
                }
                c.history_length = static_cast<std::size_t>(size);
            });
    cls.def_property(
            "full_assert", [](const Config& c) { return c.score_checks == ScoreChecks::Full; },
            [](Config& c, bool on) { c.score_checks = on ? ScoreChecks::Full : ScoreChecks::Off; });
    if constexpr (detail::HasRuinRecreateRewards<Config>::value) {
        cls.def_property(
                "ruin_recreate_rewards", [](const Config& c) { return c.ruin_recreate_rewards; },
                [](Config& c, std::vector<double> rewards) {
                    if (rewards.size() != 4) {
                        throw pybind11::value_error(
                                "ruin_recreate_rewards must contain exactly 4 values");
                    }
                    c.ruin_recreate_rewards = std::move(rewards);
                });
    }
    if constexpr (optimaru::detail::HasSolverConfigBase<Config>::value) {
        // pybind11's std::function caster re-acquires the GIL around the call,
        // so the callback is safe from a parallel start's thread too. An
        // exception it raises there is caught on that thread, stops the other
        // starts and is rethrown to Python by the pool that runs them
        // (solve_parallel_starts or the engine's solve_multistart).
        cls.def_readwrite("on_new_best", &Config::on_new_best);
        if constexpr (std::is_base_of_v<SolverConfigBase<Score>, Config>) {
            // (0, -inf) stops at the first feasible solution.
            cls.def_property(
                    "best_score_limit",
                    [](const Config& c) {
                        return pybind11::make_tuple(c.target_score.hard, c.target_score.soft);
                    },
                    [](Config& c, std::pair<std::int64_t, double> limit) {
                        c.target_score = Score{limit.first, limit.second};
                    });
        }
    }
    if constexpr (std::is_base_of_v<RuinRecreateConfig, Config>) {
        cls.def_readwrite("ruin_recreate", &Config::ruin_recreate);
        cls.def_readwrite("ruin_recreate_unimproved_step_trigger",
                          &Config::ruin_recreate_unimproved_step_trigger);
        cls.def_readwrite("ruin_recreate_min_ruin_fraction",
                          &Config::ruin_recreate_min_ruin_fraction);
        cls.def_readwrite("ruin_recreate_max_ruin_fraction",
                          &Config::ruin_recreate_max_ruin_fraction);
        cls.def_readwrite("ruin_recreate_max_ruin_count", &Config::ruin_recreate_max_ruin_count);
        cls.def_readwrite("ruin_recreate_operator_decay", &Config::ruin_recreate_operator_decay);
        cls.def_readwrite("ruin_recreate_polish_steps", &Config::ruin_recreate_polish_steps);
        if constexpr (optimaru::detail::HasAsMoveKnobs<Config>::value) {
            cls.def_readwrite("ruin_recreate_as_move_weight",
                              &Config::ruin_recreate_as_move_weight);
            cls.def_readwrite("ruin_recreate_as_move_max_ruin",
                              &Config::ruin_recreate_as_move_max_ruin);
        }
    }
    if constexpr (std::is_base_of_v<AcceptanceConfig, Config>) {
        cls.def_readwrite("acceptor_type", &Config::rule);
        cls.def_property(
                "entity_tabu_size", [](const Config& c) { return c.tabu_tenure; },
                [](Config& c, int size) {
                    if (size < 0) throw pybind11::value_error("entity_tabu_size must be >= 0");
                    c.tabu_tenure = size;
                });
        // (0, 0) = automatic: soft = the fraction below x |the start's soft score|.
        cls.def_property(
                "simulated_annealing_starting_temperature",
                [](const Config& c) {
                    return pybind11::make_tuple(c.start_temperature.hard, c.start_temperature.soft);
                },
                [](Config& c, std::pair<std::int64_t, double> temperature) {
                    c.start_temperature = detail::step_score(
                            temperature, "simulated_annealing_starting_temperature");
                });
        cls.def_property(
                "simulated_annealing_temperature_fraction",
                [](const Config& c) { return c.simulated_annealing_temperature_fraction; },
                [](Config& c, double fraction) {
                    if (!(fraction > 0.0) || !std::isfinite(fraction)) {
                        throw pybind11::value_error(
                                "simulated_annealing_temperature_fraction must be finite and > 0");
                    }
                    c.simulated_annealing_temperature_fraction = fraction;
                });
        // (0, 0) leaves the water level at the start's score.
        cls.def_property(
                "great_deluge_water_level_step",
                [](const Config& c) {
                    return pybind11::make_tuple(c.water_level_step.hard, c.water_level_step.soft);
                },
                [](Config& c, std::pair<std::int64_t, double> step) {
                    c.water_level_step = detail::step_score(step, "great_deluge_water_level_step");
                });
        cls.def_property(
                "step_counting_hill_climbing_size",
                [](const Config& c) { return c.recount_interval; },
                [](Config& c, std::int64_t size) {
                    if (size <= 0) {
                        throw pybind11::value_error(
                                "step_counting_hill_climbing_size must be positive");
                    }
                    c.recount_interval = size;
                });
        cls.def_readwrite("record_to_record_start_gap", &Config::record_to_record_start_gap);
        cls.def_readwrite("record_to_record_end_gap", &Config::record_to_record_end_gap);
    }
    return cls;
}

template <typename Class>
Class bind_solver_config(Class&& cls, bool late_acceptance_auto = true) {
    bind_solver_config(cls, late_acceptance_auto);
    return std::move(cls);
}

}  // namespace python
}  // namespace optimaru

#endif  // OPTIMARU_PYTHON_SOLVER_CONFIG_HPP

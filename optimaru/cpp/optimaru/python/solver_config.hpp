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
        throw pybind11::value_error(std::string(name) + " must be finite and >= 0, not " +
                                    shown());
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
            "__deepcopy__", [](const Config& config, const pybind11::dict&) { return Config(config); },
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
struct HasRuinRecreateRewards<Config, std::void_t<decltype(std::declval<Config&>().ruinRecreateRewards)>>
    : std::true_type {};

}  // namespace detail

// The part of SolverConfig that is the same in every use case, bound once so
// the names, the range checks and their messages cannot drift apart:
//
//   __copy__ / __deepcopy__          bind_config_copy
//   spent_limit_seconds              Config::time_limit         (seconds_to_millis)
//   unimproved_spent_limit_seconds   Config::stall_time_limit   (seconds_to_millis)
//   step_count_limit                 Config::step_limit         (0 = no limit)
//   unimproved_step_count_limit      Config::stall_step_limit   (0 = no limit)
//   random_seed                      Config::seed
//   parallel_solver_count            Config::parallelSolverCount, > 0
//   late_acceptance_size             Config::history_length, >= 0 with 0 = auto;
//                                    > 0 when `late_acceptance_auto` is false
//                                    (a use case without an automatic size)
//   full_assert                      Config::score_checks == ScoreChecks::Full
//   ruin_recreate_rewards            Config::ruinRecreateRewards, exactly 4
//                                    values (only where the Config has it)
//
// A use case binds its own fields (move weights, nearby settings,
// best_score_limit in its Score's shape, on_new_best, ...) on the class this
// returns.
template <typename Class>
Class& bind_solver_config(Class& cls, bool late_acceptance_auto = true) {
    using Config = typename Class::type;
    bind_config_copy(cls);
    cls.def_property(
            "spent_limit_seconds",
            [](const Config& c) { return std::chrono::duration<double>(c.time_limit).count(); },
            [](Config& c, double s) { c.time_limit = seconds_to_millis(s, "spent_limit_seconds"); });
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
            "parallel_solver_count", [](const Config& c) { return c.parallelSolverCount; },
            [](Config& c, int count) {
                if (count <= 0) {
                    throw pybind11::value_error("parallel_solver_count must be positive");
                }
                c.parallelSolverCount = count;
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
                "ruin_recreate_rewards", [](const Config& c) { return c.ruinRecreateRewards; },
                [](Config& c, std::vector<double> rewards) {
                    if (rewards.size() != 4) {
                        throw pybind11::value_error(
                                "ruin_recreate_rewards must contain exactly 4 values");
                    }
                    c.ruinRecreateRewards = std::move(rewards);
                });
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

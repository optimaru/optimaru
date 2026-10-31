#ifndef OPTIMARU_PYTHON_SOLVER_CONFIG_HPP
#define OPTIMARU_PYTHON_SOLVER_CONFIG_HPP

// Shared rules for every package's Python SolverConfig binding.
//
//   secondsToMillis(s, "spent_limit_seconds")
//       A time limit in seconds (spent_limit_seconds,
//       unimproved_spent_limit_seconds, ...) must be finite and >= 0, else
//       ValueError. NaN and inf used to reach duration_cast (undefined
//       behaviour; inf read back as a zero budget), and only some packages
//       refused a negative limit. 0 is a valid limit (construction only).
//
//   bindConfigCopy(py::class_<Config>(m, "SolverConfig", ...))
//       SolverConfig.__copy__ / __deepcopy__ (the C++ copy, on_new_best
//       included), so a package's solve(config=cfg, ...) can apply its
//       keyword overrides to a copy and leave the caller's config alone.

#include <pybind11/pybind11.h>

#include <chrono>
#include <cmath>
#include <string>
#include <utility>

namespace optimaru {
namespace python {

inline std::chrono::milliseconds secondsToMillis(double seconds, const char* name) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        throw pybind11::value_error(std::string(name) + " must be finite and >= 0, not " +
                                    pybind11::repr(pybind11::float_(seconds)).cast<std::string>());
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::duration<double>(seconds));
}

template <typename Class>
Class& bindConfigCopy(Class& cls) {
    using Config = typename Class::type;
    cls.def("__copy__", [](const Config& config) { return Config(config); });
    cls.def(
            "__deepcopy__", [](const Config& config, const pybind11::dict&) { return Config(config); },
            pybind11::arg("memo"));
    return cls;
}

template <typename Class>
Class bindConfigCopy(Class&& cls) {
    bindConfigCopy(cls);
    return std::move(cls);
}

}  // namespace python
}  // namespace optimaru

#endif  // OPTIMARU_PYTHON_SOLVER_CONFIG_HPP

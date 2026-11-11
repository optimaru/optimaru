// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_PYTHON_SCORE_BINDING_HPP
#define OPTIMARU_PYTHON_SCORE_BINDING_HPP

// `<pkg>._core.Score`, for a use case that reports the engine's Score or
// HardMediumSoftScore. Like the other headers here, a bindings.cpp includes
// it; no C++ target does.

#include <pybind11/pybind11.h>

#include <string>
#include <type_traits>

#include "optimaru/score/hard_medium_soft_score.hpp"
#include "optimaru/score/score.hpp"

namespace optimaru {
namespace python {

// Binds the module's Score class: the levels as read-only attributes
// (hard, soft; medium between them on a HardMediumSoftScore), is_feasible(),
// str() and repr(). Returns the class, for a use case that adds a constructor
// or comparisons:
//
//   opy::bind_score<Score>(m);
//   opy::bind_score<Score>(m).def(pybind11::self == pybind11::self);
template <typename ScoreT>
pybind11::class_<ScoreT> bind_score(pybind11::module_& m) {
    pybind11::class_<ScoreT> score(m, "Score", pybind11::module_local());
    score.def_readonly("hard", &ScoreT::hard);
    if constexpr (std::is_same_v<ScoreT, HardMediumSoftScore>) {
        score.def_readonly("medium", &ScoreT::medium);
    }
    score.def_readonly("soft", &ScoreT::soft)
            .def("is_feasible", &ScoreT::feasible)
            .def("__str__", &ScoreT::str)
            .def("__repr__", [](const ScoreT& s) { return "<Score " + s.str() + ">"; });
    return score;
}

}  // namespace python
}  // namespace optimaru

#endif  // OPTIMARU_PYTHON_SCORE_BINDING_HPP

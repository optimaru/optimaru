#ifndef OPTIMARU_PYTHON_LEVEL_GUARD_HPP
#define OPTIMARU_PYTHON_LEVEL_GUARD_HPP

// The level a Python pick's `hard` knob asks for, kept free of pybind11 so
// the engine tests can check its refusals and their wording
// (constraint_binding.hpp's wantedLevel raises them as ValueError).

#include <stdexcept>
#include <string>

#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"

namespace optimaru {
namespace python {

// hard=True asks for Hard, hard=False for Soft (a medium constraint is demoted
// to soft), checked against the component's reward / levelFlippable flags in
// the words of the Python base (_constraint.py _level):
//   constraint 'x' is a reward (raw cost <= 0) and cannot be hard
//   constraint 'x' is always hard; hard=False is not allowed
// `quotedName` is the Python name the way repr() spells it ("'x'"). Throws
// std::invalid_argument.
inline Level levelForHard(const ConstraintMetadata& constraint, bool hard,
                          const std::string& quotedName) {
    const Level wanted = hard ? Level::Hard : Level::Soft;
    if (wanted == Level::Hard && constraint.reward) {
        throw std::invalid_argument("constraint " + quotedName +
                                    " is a reward (raw cost <= 0) and cannot be hard");
    }
    if (wanted != constraint.level && !constraint.levelFlippable) {
        throw std::invalid_argument("constraint " + quotedName + " is always " +
                                    levelName(constraint.level) + "; hard=" +
                                    (hard ? "True" : "False") + " is not allowed");
    }
    return wanted;
}

}  // namespace python
}  // namespace optimaru

#endif  // OPTIMARU_PYTHON_LEVEL_GUARD_HPP

#ifndef OPTIMARU_PYTHON_LEVEL_GUARD_HPP
#define OPTIMARU_PYTHON_LEVEL_GUARD_HPP

// The knob rules of a Python pick, kept free of pybind11 so the engine tests
// can check them: the level a pick's `hard` knob asks for (its refusals and
// their wording; constraint_binding.hpp's restateKnobs callers raise them as
// ValueError), and how the five knobs, once read and type-checked
// (constraint_binding.hpp readKnobs), are re-stated on a component.

#include <optional>
#include <stdexcept>
#include <string>

#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"

namespace optimaru {
namespace python {

// The five knobs of one pick, already type-checked:
//   weight    nullopt = keep the component's default weight
//   hard      nullopt = keep the component's default level
//   exponent  always applied; a pick that leaves it unset (None) reads as
//             1.0 (no shaping), the same value the Python score() mirror
//             uses, so a component built with another exponent is reset by
//             every pick, explicit 1.0 included
//   offset    always applied, like the exponent; unset (None) reads as 0.0
//   clip      always applied, like the exponent; unset (None) reads as true
struct Knobs {
    std::optional<double> weight;
    std::optional<bool> hard;
    double exponent = 1.0;
    double offset = 0.0;
    bool clip = true;
};

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

// Re-states one pick's knobs on a component, the same way on both binding
// paths: applyConstraints (an assembly, through the overload below) and
// applyKnobs (a component a domain factory built outside any assembly).
// `quotedName` as for levelForHard. Throws std::invalid_argument.
inline void restateKnobs(ConstraintMetadata& constraint, const Knobs& knobs,
                         const std::string& quotedName) {
    if (knobs.hard) constraint.setLevel(levelForHard(constraint, *knobs.hard, quotedName));
    if (knobs.weight) constraint.setWeight(*knobs.weight);
    constraint.setExponent(knobs.exponent);
    constraint.setOffset(knobs.offset);
    constraint.setClip(knobs.clip);
}

template <typename Solution>
void restateKnobs(ConstraintAssembly<Solution>& assembly, const std::string& name,
                  const Knobs& knobs, const std::string& quotedName) {
    const ConstraintMetadata* constraint = assembly.find(name);
    if (constraint == nullptr) {
        throw std::invalid_argument("Constraint '" + name + "' is not in this assembly");
    }
    if (knobs.hard) assembly.level(name, levelForHard(*constraint, *knobs.hard, quotedName));
    if (knobs.weight) assembly.weight(name, *knobs.weight);
    assembly.exponent(name, knobs.exponent);
    assembly.offset(name, knobs.offset);
    assembly.clip(name, knobs.clip);
}

}  // namespace python
}  // namespace optimaru

#endif  // OPTIMARU_PYTHON_LEVEL_GUARD_HPP

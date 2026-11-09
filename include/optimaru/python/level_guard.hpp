// SPDX-License-Identifier: Apache-2.0
// Derived from Timefold Solver (Apache-2.0); modified by The optimaru Authors. See NOTICE.

#ifndef OPTIMARU_PYTHON_LEVEL_GUARD_HPP
#define OPTIMARU_PYTHON_LEVEL_GUARD_HPP

// The knob rules of a Python pick, kept free of pybind11 so the engine tests
// can check them: whether a pick's `level` knob may be honoured (checkedLevel)
// and whether its offset / clip may sit on its effective level
// (checkLiteralOffset), with their refusals and wording (constraint_binding.hpp's
// restateKnobs callers raise them as ValueError), and how the five knobs, once
// read and type-checked (constraint_binding.hpp readKnobs), are re-stated on a
// component.

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"

namespace optimaru {
namespace python {

// The five knobs of one pick, already type-checked:
//   weight    nullopt = keep the component's default weight
//   level     nullopt = keep the component's default level
//   exponent  always applied; a pick that leaves it unset (None) reads as
//             1.0 (no shaping), the same value the Python score() mirror
//             uses, so a component built with another exponent is reset by
//             every pick, explicit 1.0 included
//   offset    always applied, like the exponent; unset (None) reads as 0.0
//   clip      always applied, like the exponent; unset (None) reads as true
struct Knobs {
    std::optional<double> weight;
    std::optional<Level> level;
    double exponent = 1.0;
    double offset = 0.0;
    bool clip = true;
};

// The Level a pick's name spells ("hard", "medium", "soft"), or nullopt.
inline std::optional<Level> levelNamed(std::string_view name) {
    for (const Level level : {Level::Hard, Level::Medium, Level::Soft}) {
        if (name == levelName(level)) return level;
    }
    return std::nullopt;
}

// Checks a pick's level against the component's reward / levelFlippable flags
// and the domain's medium fold, in the words of the Python base
// (_constraint.py _level):
//   constraint 'x': this use case has no medium level
//   constraint 'x' is a reward (raw cost <= 0) and cannot be hard
//   constraint 'x' is a reward (raw cost <= 0) and cannot be medium here, ...
//   constraint 'x' is always hard; level='soft' is not allowed
// `quotedName` is the Python name the way repr() spells it ("'x'"). `constraint`
// must still be at its default level, as it is on both binding paths, which is
// what makes constraint.level here the same as _level's default. Throws
// std::invalid_argument.
inline Level checkedLevel(const ConstraintMetadata& constraint, Level wanted, MediumFold fold,
                          const std::string& quotedName) {
    if (wanted == Level::Medium && !fold.hasMedium()) {
        throw std::invalid_argument("constraint " + quotedName +
                                    ": this use case has no medium level");
    }
    if (constraint.reward && wanted == Level::Hard) {
        throw std::invalid_argument("constraint " + quotedName +
                                    " is a reward (raw cost <= 0) and cannot be hard");
    }
    if (constraint.reward && wanted == Level::Medium && fold.roundsMedium()) {
        throw std::invalid_argument("constraint " + quotedName +
                                    " is a reward (raw cost <= 0) and cannot be medium here, "
                                    "where medium counts toward the hard score");
    }
    if (wanted != constraint.level && !constraint.levelFlippable) {
        throw std::invalid_argument("constraint " + quotedName + " is always " +
                                    levelName(constraint.level) + "; level='" + levelName(wanted) +
                                    "' is not allowed");
    }
    return wanted;
}

// The hard-0 rule, stated here once (the other comments refer here): a level
// that counts toward the hard score (hard, or medium where it folds below
// hard: MediumFold::countsTowardHard) refuses clip=False with a non-zero
// offset, because hard 0 must mean no violation. The literal formula charges
// every evaluated unit shapeCost(x - offset), a raw 0 included: a positive
// offset rewards the units below it, so one unit's slack would cancel
// another's violation; a negative one charges units without any violation.
// The level is the pick's, or the component's default when the pick leaves
// it unset, a structural (non-flippable) hard one included.
//
// This checks a pick once ALL of its knobs are re-stated (so the order of the
// setters does not matter), on the component's effective state, in the
// words of the Python base (_constraint.py _check_literal_offset):
//   constraint 'x': clip=False with a non-zero offset cannot be hard, because
//       hard 0 must mean no violation; use clip=True (a threshold) or another
//       level
//   constraint 'x': clip=False with a non-zero offset cannot be medium here,
//       where medium counts toward the hard score; use clip=True (a
//       threshold) or level='soft'
// The threshold (clip=True) with any offset, clip=False with offset 0, and
// any combination on soft or on an into_soft medium stay allowed. The engine
// refuses the same states at reset (DecomposedScoreCalculator::
// validateLevel, std::logic_error); this is the Python-facing wording.
// `quotedName` as for checkedLevel. Throws std::invalid_argument.
inline void checkLiteralOffset(const ConstraintMetadata& constraint, MediumFold fold,
                               const std::string& quotedName) {
    if (constraint.clip || constraint.offset == 0.0 || !fold.countsTowardHard(constraint.level)) {
        return;
    }
    if (constraint.level == Level::Hard) {
        throw std::invalid_argument("constraint " + quotedName +
                                    ": clip=False with a non-zero offset cannot be hard, because "
                                    "hard 0 must mean no violation; use clip=True (a threshold) or "
                                    "another level");
    }
    throw std::invalid_argument("constraint " + quotedName +
                                ": clip=False with a non-zero offset cannot be medium here, where "
                                "medium counts toward the hard score; use clip=True (a threshold) "
                                "or level='soft'");
}

// Re-states one pick's knobs on a component, the same way on both binding
// paths: applyConstraints (an assembly, through the overload below) and
// applyKnobs (a component a domain factory built outside any assembly), then
// checks the result (checkLiteralOffset). `quotedName` as for checkedLevel;
// `fold` is the domain's. Throws std::invalid_argument.
inline void restateKnobs(ConstraintMetadata& constraint, const Knobs& knobs, MediumFold fold,
                         const std::string& quotedName) {
    if (knobs.level) constraint.setLevel(checkedLevel(constraint, *knobs.level, fold, quotedName));
    if (knobs.weight) constraint.setWeight(*knobs.weight);
    constraint.setExponent(knobs.exponent);
    constraint.setOffset(knobs.offset);
    constraint.setClip(knobs.clip);
    checkLiteralOffset(constraint, fold, quotedName);
}

template <typename Solution>
void restateKnobs(ConstraintAssembly<Solution>& assembly, const std::string& name,
                  const Knobs& knobs, const std::string& quotedName) {
    const ConstraintMetadata* constraint = assembly.find(name);
    if (constraint == nullptr) {
        throw std::invalid_argument("Constraint '" + name + "' is not in this assembly");
    }
    if (knobs.level) {
        assembly.level(name, checkedLevel(*constraint, *knobs.level, MediumFoldOf<Solution>::value,
                                          quotedName));
    }
    if (knobs.weight) assembly.weight(name, *knobs.weight);
    assembly.exponent(name, knobs.exponent);
    assembly.offset(name, knobs.offset);
    assembly.clip(name, knobs.clip);
    checkLiteralOffset(*constraint, MediumFoldOf<Solution>::value, quotedName);
}

}  // namespace python
}  // namespace optimaru

#endif  // OPTIMARU_PYTHON_LEVEL_GUARD_HPP

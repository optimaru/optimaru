#ifndef OPTIMARU_PYTHON_CONSTRAINT_BINDING_HPP
#define OPTIMARU_PYTHON_CONSTRAINT_BINDING_HPP

// ---------------------------------------------------------------------------
// Python constraint binding
// ---------------------------------------------------------------------------
// The pybind11 side of the declarative cost function: what every use case's
// `bindings.cpp` needs to turn a list of Python constraint specs into a
// ConstraintAssembly. The engine itself does not depend on pybind11 — this
// header lives under core/include only because every package's setup.py
// already puts that directory on the include path, and it is pulled in by
// bindings.cpp alone, never by a C++ target.
//
//   ConstraintAssembly assembly = python::applyConstraints(
//       defaultConstraints(), constraints);   // or defaultConstraints(problem)
//   assembly.into(calculator);
//
// The domain builds its own default assembly and hands it over, so the fact
// that `defaultConstraints()` takes a problem in one use case, weights in
// another and nothing in the rest never reaches this header.

#include <pybind11/pybind11.h>

#include <cctype>
#include <string>
#include <vector>

#include "optimaru/api/score/calculator/constraint_assembly.hpp"

namespace optimaru {
namespace python {

namespace py = pybind11;

// The Python-facing spelling of a constraint name. The C++ components carry
// names in the shape their provider used ("facility setup cost",
// "sameBedInSameNight"); the Python packages are snake_case, so derive one
// from the other MECHANICALLY rather than hand-writing an alias table that
// would drift the first time a component is added or renamed.
//
// One shape it does not handle: a run of capitals is broken up letter by
// letter ("HTTPTag" -> "h_t_t_p_tag"), because the rule cannot tell an
// acronym from adjacent words without a dictionary. No component name has
// one today; a new constraint that needs an acronym should spell it in the
// C++ name the way it should read in Python.
inline std::string pythonName(const std::string& constraintName) {
    std::string out;
    for (const char c : constraintName) {
        if (c == ' ' || c == '-') {
            if (!out.empty() && out.back() != '_') out += '_';
        } else if (std::isupper(static_cast<unsigned char>(c))) {
            if (!out.empty() && out.back() != '_') out += '_';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            out += c;
        }
    }
    return out;
}

// Python name -> the assembly's own name. Throws with the available names
// when nothing matches, so a typo cannot quietly leave the cost function as
// it was.
template <typename Solution>
std::string resolveName(const ConstraintAssembly<Solution>& assembly, const std::string& wanted) {
    for (const std::string& name : assembly.names()) {
        if (pythonName(name) == wanted) return name;
    }
    std::string known;
    for (const std::string& name : assembly.names()) {
        known += (known.empty() ? "" : ", ") + ("'" + pythonName(name) + "'");
    }
    throw py::value_error("unknown constraint '" + wanted + "'; available: " + known);
}

// The constraint names a domain scores, in default-assembly order, spelled the
// way Python asks for them — what `list_constraints()` returns.
template <typename Solution>
std::vector<std::string> constraintNames(const ConstraintAssembly<Solution>& assembly) {
    std::vector<std::string> names;
    for (const std::string& name : assembly.names()) names.push_back(pythonName(name));
    return names;
}

// The raw per-unit costs of a domain's default assembly, primed on a
// solution: what every constraint's cost() returned, before the weight, the
// exponent and the hard rounding.
//
// This is the whole engine-side half of `<pkg>.constraints.score()`. Turning
// those numbers into a score is weight/level/exponent arithmetic that does
// not depend on the domain OR on the search, so it lives in Python instead
// of here — one implementation, readable, and re-pricing an assembly costs no
// scoring pass at all.
//
// Returns {name: {"costs": [...], "weight": w, "exponent": e, "hard": bool,
//                 "global": bool}}.
template <typename Solution>
py::dict constraintCosts(DecomposedScoreCalculator<Solution>& calculator, Solution& solution) {
    calculator.resetWorkingSolution(solution);
    calculator.calculateScore();  // primes the per-unit cache
    py::dict out;
    for (const auto& entry : calculator.constraintCosts()) {
        py::dict info;
        info["costs"] = py::cast(entry.unitCosts);
        info["weight"] = entry.weight;
        info["exponent"] = entry.exponent;
        info["hard"] = entry.isHard;
        info["global"] = entry.isGlobal;
        out[py::str(pythonName(entry.name))] = info;
    }
    return out;
}

// Applies the constraint picks coming from Python (<pkg>.constraints) to a
// domain's default assembly: the name of a component plus the metadata to
// re-state on it. Duck-typed on purpose — any object carrying
// name/weight/hard/exponent works, which is what lets the spec dataclasses
// stay plain Python.
//
// The picks REPLACE the default set: passing three of them scores exactly
// those three. Passing None keeps every default; an empty list is an error,
// not "the defaults" (a filter that came back empty is a mistake, and the
// Python get_constraints() rejects it for the same reason). Naming the same
// constraint twice is an error too. Since the assembly already looks
// components up by name, there is no name -> factory registry here to drift
// out of sync.
template <typename Solution>
ConstraintAssembly<Solution> applyConstraints(ConstraintAssembly<Solution> assembly,
                                              const py::object& constraints) {
    if (constraints.is_none()) return assembly;
    std::vector<std::string> picked;
    for (py::handle item : constraints) {
        const std::string name = resolveName(assembly, item.attr("name").template cast<std::string>());
        const py::object weight = item.attr("weight");
        if (!weight.is_none()) assembly.weight(name, weight.template cast<double>());
        const py::object hard = item.attr("hard");
        if (!hard.is_none()) {
            if (hard.template cast<bool>()) {
                assembly.hard(name);
            } else {
                assembly.soft(name);
            }
        }
        // Read like weight/hard: the picks are duck-typed, so a spec that
        // leaves the exponent unset (or None) means "no shaping" rather than
        // a cast error out of pybind11.
        const py::object exponent = item.attr("exponent");
        if (!exponent.is_none()) {
            const double value = exponent.template cast<double>();
            if (value != 1.0) assembly.exponent(name, value);
        }
        // The same constraint twice would let the second pick's metadata win
        // in silence — the same class of mistake as a typo'd name, so it is
        // an error rather than a last-wins rule nobody can see.
        for (const std::string& already : picked) {
            if (already == name) {
                throw py::value_error("constraint '" + pythonName(name)
                                      + "' picked more than once; state it once with the "
                                        "metadata you want");
            }
        }
        picked.push_back(name);
    }
    if (picked.empty()) {
        throw py::value_error("constraints must not be empty (pass None for the defaults)");
    }
    assembly.only(picked);
    return assembly;
}

}  // namespace python
}  // namespace optimaru

#endif  // OPTIMARU_PYTHON_CONSTRAINT_BINDING_HPP

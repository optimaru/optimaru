#ifndef OPTIMARU_PYTHON_CONSTRAINT_BINDING_HPP
#define OPTIMARU_PYTHON_CONSTRAINT_BINDING_HPP

// ---------------------------------------------------------------------------
// Python constraint binding
// ---------------------------------------------------------------------------
// The pybind11 side of the declarative cost function: what every use case's
// `bindings.cpp` needs to turn a list of Python constraint specs into a
// ConstraintAssembly. The engine itself does not depend on pybind11 — this
// header lives under the engine's include/ only because every package's
// setup.py already puts that directory on the include path, and it is
// pulled in by bindings.cpp alone, never by a C++ target.
//
//   ConstraintAssembly assembly = python::applyConstraints(
//       defaultConstraints(), constraints);   // or defaultConstraints(problem)
//   assembly.into(calculator);
//
//   python::bindConstraintInfo(m, [] { return defaultConstraints(); });
//   // -> <pkg>.constraint_info() and <pkg>._core.ConstraintInfo
//
// A domain whose factory builds components outside any assembly (a rule
// list, employee-scheduling addConstraintByName) re-states each component's
// knobs with python::applyKnobs(component, pick, shownName) and reports its
// wider catalog with the three-argument bindConstraintInfo (see there).
//
// The domain builds its own default assembly and hands it over, so the fact
// that `defaultConstraints()` takes a problem in one use case, weights in
// another and nothing in the rest never reaches this header.
//
// bindConstraintInfo() also registers the module's translator for the
// engine's ContributionOverflowError (level.hpp, rule 1), so every package
// raises it as OverflowError naming the constraint by its Python name, word
// for word as the Python score() mirror does, and the private test seam
// `<pkg>._core._fail_worker_thread_start(index)` (see bindWorkerThreadSeam).

#include <pybind11/pybind11.h>

#include <cmath>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/impl/solver/worker_threads.hpp"
#include "optimaru/python/level_guard.hpp"
#include "optimaru/python/names.hpp"
#include "optimaru/python/solver_config.hpp"  // secondsToMillis, bindConfigCopy

namespace optimaru {
namespace python {

namespace py = pybind11;

// pythonName() (names.hpp) is the one mapping between the C++ registry name
// and the Python name: facilityCapacityCost -> facility_capacity_cost.

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

// The raw per-unit costs of an assembly, primed on a solution: what every
// constraint's cost() returned, before the weight, the exponent and the hard
// rounding.
//
// This is the whole engine-side half of `<pkg>.constraints.score()`. Turning
// those numbers into a score is weight/level/exponent arithmetic that does
// not depend on the domain OR on the search, so it lives in Python instead
// of here — one implementation, readable, and re-pricing an assembly costs no
// scoring pass at all.
//
// Returns {key: {"costs": [...], "weight": w, "exponent": e, "hard": bool,
//                "global": bool, "level": "hard"|"medium"|"soft",
//                "reward": bool, "level_flippable": bool, "name": str,
//                "label": str, "default_weight": w0,
//                "default_level": "hard"|"medium"|"soft"
//                [, "medium_fold": {...}]}}.
//
// key    the Python name; the k-th instance (k >= 2) of a name the calculator
//        holds more than once is "<name>#<k>" (instanceKeys, names.hpp), so
//        repeated parametrized rules do not overwrite each other. "name" is
//        always the plain Python name, "label" the instance's display text
//        (ConstraintMetadata::label; "" = none).
// medium_fold  {"kind": "into_soft" | "below_hard", "unit": u}: the DOMAIN's
//        MediumFold, on EVERY entry of a domain that has one (whatever level
//        the entry is on), because the fold applies to the whole score: a
//        below_hard domain reports hard * unit + medium even when the
//        selection holds no medium constraint at all. Absent on a domain
//        without a medium level.
// weight / level / exponent  the calculator's CURRENT metadata, which is the
//        picks' when the domain's constraint_costs() takes a selection
//        (employee-scheduling, school-timetabling).
// default_weight / default_level  what weight=None / level=None mean for the
//        component (ConstraintMetadata::defaultWeight / defaultLevel): the
//        Python score() reads these, so re-pricing costs computed under one
//        selection with another leaves an unset knob at the real default.
// "hard" stays for pre-0.2.0 readers (it is level == "hard").
template <typename Solution>
py::dict constraintCosts(DecomposedScoreCalculator<Solution>& calculator, Solution& solution) {
    calculator.resetWorkingSolution(solution);
    calculator.calculateScore();  // primes the per-unit cache
    constexpr MediumFold fold = DecomposedScoreCalculator<Solution>::kMediumFold;
    const auto entries = calculator.constraintCosts();
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const auto& entry : entries) names.push_back(pythonName(entry.name));
    const std::vector<std::string> keys = instanceKeys(names);
    py::dict out;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        py::dict info;
        info["costs"] = py::cast(entry.unitCosts);
        info["weight"] = entry.weight;
        info["exponent"] = entry.exponent;
        info["hard"] = entry.isHard;
        info["global"] = entry.isGlobal;
        info["level"] = levelName(entry.level);
        info["reward"] = entry.reward;
        info["level_flippable"] = entry.levelFlippable;
        info["name"] = names[i];
        info["label"] = entry.label;
        info["default_weight"] = entry.defaultWeight;
        info["default_level"] = levelName(entry.defaultLevel);
        if (fold.hasMedium()) {
            py::dict medium;
            medium["kind"] = fold.kind == MediumFold::Kind::BelowHard ? "below_hard" : "into_soft";
            medium["unit"] = fold.unit;
            info["medium_fold"] = medium;
        }
        out[py::str(keys[i])] = info;
    }
    return out;
}

// ---------------------------------------------------------------------------
// The three knobs of one pick, read the way the Python base reads them
// ---------------------------------------------------------------------------
// The picks are duck-typed (any object with name/weight/level/exponent), so
// the C++ side checks the types itself, with the same rules and the same
// wording as `_constraint.py` (_check_knobs / _level): what one path refuses,
// the other refuses too.
//   weight    None, or a real number (numbers.Real, but not bool), finite
//             and >= 0. TypeError / ValueError.
//   level     None, or a string spelling a level ("hard", "medium", "soft"),
//             which a Python Level is. TypeError / ValueError.
//   exponent  None (= 1.0, no shaping: a duck-typed pick may leave it unset),
//             or a real number (not bool), finite and > 0.
// The result is a Knobs (level_guard.hpp), which restateKnobs applies.

namespace detail {

inline std::string quoted(const std::string& shown) {
    return py::repr(py::str(shown)).cast<std::string>();
}

inline std::string reprOf(const py::handle& value) {
    return py::repr(value).cast<std::string>();
}

// numbers.Real minus bool: what _check_knobs accepts as a weight/exponent.
// numbers.Real is looked up once per module (gil_safe_call_once_and_store,
// which also keeps it alive until the interpreter is gone), not per knob.
inline bool isRealNumber(const py::handle& value) {
    if (py::isinstance<py::bool_>(value)) return false;
    PYBIND11_CONSTINIT static py::gil_safe_call_once_and_store<py::object> numbersReal;
    const py::object& real =
            numbersReal
                    .call_once_and_store_result(
                            [] { return py::module_::import("numbers").attr("Real"); })
                    .get_stored();
    return py::isinstance(value, real);
}

}  // namespace detail

// Reads and checks one pick's knobs; `shown` is the Python name used in the
// messages.
inline Knobs readKnobs(const py::handle& item, const std::string& shown) {
    Knobs knobs;
    const std::string label = "constraint " + detail::quoted(shown);
    const py::object weight = item.attr("weight");
    if (!weight.is_none()) {
        if (!detail::isRealNumber(weight)) {
            throw py::type_error(label + ": weight must be a number or None, not " +
                                 detail::reprOf(weight));
        }
        const double value = py::float_(weight).cast<double>();
        if (!std::isfinite(value) || value < 0.0) {
            throw py::value_error(label + ": weight must be finite and >= 0, not " +
                                  detail::reprOf(weight));
        }
        knobs.weight = value;
    }
    const py::object level = item.attr("level");
    if (!level.is_none()) {
        if (!py::isinstance<py::str>(level)) {
            throw py::type_error(label + ": level must be a Level, its name or None, not " +
                                 detail::reprOf(level));
        }
        knobs.level = levelNamed(level.cast<std::string>());
        if (!knobs.level) {
            throw py::value_error(label + ": level must be 'hard', 'medium' or 'soft', not " +
                                  detail::reprOf(level));
        }
    }
    const py::object exponent = item.attr("exponent");
    if (!exponent.is_none()) {
        if (!detail::isRealNumber(exponent)) {
            throw py::type_error(label + ": exponent must be a number, not " +
                                 detail::reprOf(exponent));
        }
        const double value = py::float_(exponent).cast<double>();
        if (!std::isfinite(value) || value <= 0.0) {
            throw py::value_error(label + ": exponent must be finite and > 0, not " +
                                  detail::reprOf(exponent));
        }
        knobs.exponent = value;
    }
    return knobs;
}

// Re-states one pick's knobs on a component a domain factory built (with
// its kDefaultWeight / kDefaultLevel, so None keeps them): the same checks
// and the same rule (restateKnobs, level_guard.hpp; the exponent is always
// applied) as applyConstraints, for code that cannot go through an assembly
// (employee-scheduling's rule factory, which may add a rule several times).
// Apply it to the OUTERMOST component — after wrapping in a ScopedUnit, which
// copies the inner component's metadata.
//
//   auto unit = std::make_unique<MaxConsecutiveDaysCost>(
//           MaxConsecutiveDaysCost::kDefaultWeight, value);
//   python::applyKnobs(*unit, pick, "max_consecutive_days_cost");
inline void applyKnobs(ConstraintMetadata& constraint, const py::handle& item,
                       const std::string& shown, MediumFold fold = MediumFold::none()) {
    const Knobs knobs = readKnobs(item, shown);
    try {
        restateKnobs(constraint, knobs, fold, detail::quoted(shown));
    } catch (const std::invalid_argument& error) {
        throw py::value_error(error.what());
    }
}

// Applies the constraint picks coming from Python (<pkg>.constraints) to a
// domain's default assembly: the name of a component plus the metadata to
// re-state on it. Duck-typed on purpose — any object carrying
// name/weight/level/exponent works, which is what lets the spec dataclasses
// stay plain Python. The three knobs:
//   weight    None = the component's kDefaultWeight (in units of its level),
//             else a finite number >= 0
//   level     None = the component's kDefaultLevel, otherwise "hard",
//             "medium" or "soft" (a Python Level). See checkedLevel for what
//             a domain or a constraint refuses.
//   exponent  1.0 = no shaping, else finite and > 0; always applied, so an
//             explicit 1.0 (or None) resets a default assembly component
//             built with another exponent, as the Python score() reads it
// (readKnobs above). A wrong type is a TypeError, every other refusal a
// ValueError, each naming the constraint by its Python name, in the words
// of the Python base.
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
        const std::string shown = pythonName(name);
        const Knobs knobs = readKnobs(item, shown);
        try {
            restateKnobs(assembly, name, knobs, detail::quoted(shown));
        } catch (const std::invalid_argument& error) {
            throw py::value_error(error.what());
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

// Rule 1 of "The range of a score" (level.hpp) in Python's words: the
// constraint's Python name and Python's repr() of the contribution, so a
// solve and score(constraint_costs(...), picks) refuse the same pick with
// the same OverflowError text. Module-local: each package's extension
// translates the exceptions its own copy of the engine throws.
inline void registerContributionOverflowTranslator() {
    py::register_local_exception_translator([](std::exception_ptr pending) {
        try {
            if (pending) std::rethrow_exception(pending);
        } catch (const ContributionOverflowError& error) {
            const std::string shown = py::repr(py::float_(error.value())).cast<std::string>();
            const std::string text = ContributionOverflowError::message(
                    pythonName(error.constraint()), shown, error.beyondHardLimit());
            PyErr_SetString(PyExc_OverflowError, text.c_str());
        }
    });
}

// Binds `<pkg>._core.constraint_info()` and its row type
// `<pkg>._core.ConstraintInfo`:
//
//   ConstraintInfo(name='facility_capacity_cost', default_level='hard',
//                  default_weight=1.0, level_flippable=True, reward=False,
//                  default_enabled=True)
//
//   bindConstraintInfo(m, defaultAssembly)
//       one row per constraint of the DEFAULT assembly, in list_constraints()
//       order; default_enabled = default_weight > 0.
//   bindConstraintInfo(m, defaultAssembly, catalogAssembly)
//       for a domain that builds more constraints than it scores by default
//       (employee-scheduling): catalogInfo() — the default rows, then one row
//       per catalog-only constraint with default_enabled=False. Every name
//       the domain's factory accepts must be in one of the two assemblies, so
//       the server catalog and the CP-SAT plugins (cpsat-common reads these
//       rows) find each of them.
//
// Each argument is a callable returning a ConstraintAssembly (a lambda, so a
// domain whose factory takes arguments passes a neutral problem). The rows
// are a collections.namedtuple created here and registered on the module, so
// they are plain values: attribute access, ==, repr, _asdict() and pickling
// (by reference to <pkg>._core.ConstraintInfo) all work without a pybind11
// class, which also keeps them identical in a standalone package and in the
// optimaru wheel.
namespace detail {

// The worker the test seam fails, -1 = none (module-local, like the hook).
inline int& failingWorkerThread() {
    static int index = -1;
    return index;
}

inline void failWorkerThreadStart(int index) {
    if (index == failingWorkerThread()) {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                "injected worker thread creation failure");
    }
}

// `<pkg>._core._fail_worker_thread_start(index)`: a private test seam over
// optimaru::workerThreadStartHook (impl/solver/worker_threads.hpp). From the
// next parallel solve on, creating worker thread `index` of every pool in this
// module fails with std::system_error ("injected worker thread creation
// failure", a RuntimeError in Python), as std::thread does when the system
// has no thread left; -1 switches it off again. The package tests use it to
// check that such a solve stops and joins the workers already running and
// raises, where it used to call std::terminate. Not for production use.
inline void bindWorkerThreadSeam(py::module_& m) {
    m.def(
            "_fail_worker_thread_start",
            [](int index) {
                failingWorkerThread() = index;
                workerThreadStartHook = index < 0 ? nullptr : &failWorkerThreadStart;
            },
            py::arg("index"),
            "Test seam: fail the creation of worker thread `index` in every parallel "
            "solve from now on (-1 = off).");
}

template <typename Rows>
void bindInfoRows(py::module_& m, Rows rowsOf) {
    registerContributionOverflowTranslator();
    bindWorkerThreadSeam(m);
    py::object namedtuple = py::module_::import("collections").attr("namedtuple");
    py::object rowType = namedtuple(
            "ConstraintInfo",
            py::make_tuple("name", "default_level", "default_weight", "level_flippable", "reward",
                           "default_enabled"));
    rowType.attr("__module__") = m.attr("__name__");
    rowType.attr("__doc__") =
            "One catalog row: the constraint's Python name, its default level "
            "('hard' | 'medium' | 'soft'), its default weight (in units of that "
            "level), whether a pick's level may change it, whether it is a "
            "reward (raw cost <= 0; level='hard' is refused), and whether it is "
            "scored when no constraints are named (in the default set at a "
            "default weight > 0).";
    m.attr("ConstraintInfo") = rowType;
    m.def(
            "constraint_info",
            [rowType, rowsOf]() {
                py::list rows;
                for (const ConstraintInfo& row : rowsOf()) {
                    rows.append(rowType(pythonName(row.name), levelName(row.defaultLevel),
                                        row.defaultWeight, row.levelFlippable, row.reward,
                                        row.defaultEnabled));
                }
                return rows;
            },
            "The constraint catalog: one ConstraintInfo(name, default_level, "
            "default_weight, level_flippable, reward, default_enabled) per "
            "constraint, the list_constraints() defaults first, read off the C++ "
            "components' kDefaultWeight / kDefaultLevel.");
}

}  // namespace detail

template <typename Factory>
void bindConstraintInfo(py::module_& m, Factory defaultAssembly) {
    detail::bindInfoRows(m, [defaultAssembly]() { return defaultAssembly().info(); });
}

template <typename DefaultFactory, typename CatalogFactory>
void bindConstraintInfo(py::module_& m, DefaultFactory defaultAssembly,
                        CatalogFactory catalogAssembly) {
    // catalogInfo's std::logic_error (a default declared differently in the
    // two assemblies: a domain bug) reaches Python as RuntimeError.
    detail::bindInfoRows(m, [defaultAssembly, catalogAssembly]() {
        return catalogInfo(defaultAssembly(), catalogAssembly());
    });
}

}  // namespace python
}  // namespace optimaru

#endif  // OPTIMARU_PYTHON_CONSTRAINT_BINDING_HPP

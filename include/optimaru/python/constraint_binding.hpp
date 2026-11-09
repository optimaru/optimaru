// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

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
//   ConstraintAssembly assembly = python::apply_constraints(
//       [&] { return defaultConstraints(); }, constraints);   // or (problem)
//   assembly.into(objective);
//
//   python::bind_constraint_info(m, [] { return defaultConstraints(); });
//   // -> <pkg>.constraint_info() and <pkg>._core.ConstraintInfo
//
// A domain whose factory builds components outside any assembly (a rule
// list, employee-scheduling addConstraintByName) re-states each component's
// knobs with python::apply_knobs(component, pick, shownName, fold) and reports its
// wider catalog with the three-argument bind_constraint_info (see there).
//
// The domain builds its own default assembly and hands it over, so the fact
// that `defaultConstraints()` takes a problem in one use case, weights in
// another and nothing in the rest never reaches this header.
//
// bind_constraint_info() also registers the module's translator for the
// engine's ContributionOverflowError (levels.hpp, rule 1), so every package
// raises it as OverflowError naming the constraint by its Python name, word
// for word as the Python score() mirror does (and SignedCostLevelError as its
// ValueError), and the private test seam
// `<pkg>._core._fail_worker_thread_start(index)` (see bind_worker_thread_seam).

#include <pybind11/pybind11.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/worker_threads.hpp"
#include "optimaru/python/level_guard.hpp"
#include "optimaru/python/names.hpp"
#include "optimaru/python/solver_config.hpp"  // seconds_to_millis, bind_config_copy

namespace optimaru {
namespace python {

namespace py = pybind11;

// python_name() (names.hpp) is the one mapping between the C++ registry name
// and the Python name: facilityCapacityCost -> facility_capacity_cost.

// The TypeError text for a `hard` keyword on a binding that reads its other
// keywords as domain parameters (employee scheduling's
// DecomposedCalculator.add()), where an unread keyword would otherwise be
// ignored: the level is the `level` knob.
inline constexpr const char* kHardKeyword =
        "unexpected keyword argument 'hard'; use level='hard' or level='soft' (level=None keeps "
        "the default)";

// Refuses a `hard` keyword with kHardKeyword; call it first on the **kwargs
// of a binding that reads its keywords as domain parameters.
inline void refuse_hard_keyword(const py::kwargs& kwargs) {
    if (kwargs.contains("hard")) throw py::type_error(kHardKeyword);
}

// Python name -> the assembly's own name. Throws with the available names
// when nothing matches, so a typo cannot quietly leave the cost function as
// it was.
template <typename Solution>
std::string resolve_name(const ConstraintAssembly<Solution>& assembly, const std::string& wanted) {
    for (const std::string& name : assembly.names()) {
        if (python_name(name) == wanted) return name;
    }
    std::string known;
    for (const std::string& name : assembly.names()) {
        known += (known.empty() ? "" : ", ") + ("'" + python_name(name) + "'");
    }
    throw py::value_error("unknown constraint '" + wanted + "'; available: " + known);
}

// The constraint names a domain scores, in default-assembly order, spelled the
// way Python asks for them — what `list_constraints()` returns.
template <typename Solution>
std::vector<std::string> constraint_names(const ConstraintAssembly<Solution>& assembly) {
    std::vector<std::string> names;
    for (const std::string& name : assembly.names()) names.push_back(python_name(name));
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
// Returns {key: {"costs": [...], "weight": w, "exponent": e, "offset": o,
//                "clip": bool, "global": bool,
//                "level": "hard"|"medium"|"soft",
//                "reward": bool, "level_flippable": bool, "name": str,
//                "label": str, "default_weight": w0,
//                "default_level": "hard"|"medium"|"soft"
//                [, "units": [...]] [, "medium_fold": {...}]}}.
//
// key    the Python name; the k-th instance (k >= 2) of a name the objective
//        holds more than once is "<name>#<k>" (instance_keys, names.hpp), so
//        repeated picks (and repeated parametrized rules) do not overwrite
//        each other. "name" is always the plain Python name, "label" the
//        instance's display text (ConstraintMetadata::label; "" = none).
// costs  one raw cost per unit (every unit of the domain, in unit order), or
//        the single total of a global constraint.
// units  only on a SCOPED unit constraint (scoped_units(), e.g. a rule for a
//        few workers): the unit indices it evaluates, ascending. The other
//        cells of "costs" are 0 and never evaluated, which matters for the
//        literal offset formula (clip=False): it charges every evaluated
//        unit, a raw 0 included, and no other. Absent = every unit.
// medium_fold  {"kind": "into_soft" | "below_hard", "unit": u}: the DOMAIN's
//        MediumFold, on EVERY entry of a domain that has one (whatever level
//        the entry is on), because the fold applies to the whole score: a
//        below_hard domain reports hard * unit + medium even when the
//        selection holds no medium constraint at all. Absent on a domain
//        without a medium level.
// weight / level / exponent / offset / clip  the objective's CURRENT
//        metadata, which is the picks' when the domain's constraint_costs()
//        takes a selection.
// default_weight / default_level  what weight=None / level=None mean for the
//        component (ConstraintMetadata::default_weight / default_level): the
//        Python score() reads these, so re-pricing costs computed under one
//        selection with another leaves an unset knob at the real default.
template <typename Solution>
py::dict constraint_costs(DecomposedObjective<Solution>& objective, Solution& solution) {
    objective.rebuild(solution);
    objective.score();  // primes the per-unit cache
    constexpr MediumFold fold = DecomposedObjective<Solution>::kMediumFold;
    const auto entries = objective.constraint_costs();
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const auto& entry : entries) names.push_back(python_name(entry.name));
    const std::vector<std::string> keys = instance_keys(names);
    py::dict out;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        py::dict info;
        info["costs"] = py::cast(entry.unit_costs);
        info["weight"] = entry.weight;
        info["exponent"] = entry.exponent;
        info["offset"] = entry.offset;
        info["clip"] = entry.clip;
        info["global"] = entry.is_global;
        info["level"] = level_name(entry.level);
        info["reward"] = entry.reward;
        info["level_flippable"] = entry.level_flippable;
        info["name"] = names[i];
        info["label"] = entry.label;
        info["default_weight"] = entry.default_weight;
        info["default_level"] = level_name(entry.default_level);
        if (entry.scoped) info["units"] = py::cast(entry.units);
        // The sign rule's input (SignedCostLevelError): only where it holds,
        // so every other entry stays as it was.
        if (entry.signed_cost) info["signed"] = true;
        if (fold.has_medium()) {
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
// The five knobs of one pick, read the way the Python base reads them
// ---------------------------------------------------------------------------
// The picks are duck-typed, so the C++ side checks the types itself, with the
// same rules and the same wording as `_constraint.py` (_check_knobs /
// _level): what one path refuses, the other refuses too. The duck-typing
// rule, in the same words as the Python score_costs docstring:
//   A pick must carry `name`, `weight`, `level` and `exponent`. `offset` and
//   `clip` are optional and default to 0.0 and True. They only add shaping,
//   and their defaults change nothing. A missing weight, level or exponent
//   stays an AttributeError, because it is more likely a mistake, e.g. an
//   object still carrying `hard`.
//   weight    None, or a real number (numbers.Real, but not bool), finite
//             and >= 0. TypeError / ValueError.
//   level     None, or a string spelling a level ("hard", "medium", "soft"),
//             which a Python Level is. TypeError / ValueError.
//   exponent  None (= 1.0, no shaping: a duck-typed pick may leave it unset),
//             or a real number (not bool), finite and > 0.
//   offset    absent or None (= 0.0, no shift), or a real number (not bool),
//             finite, any sign.
//   clip      absent or None (= True, the threshold), or True / False.
// A numeric knob counts as the double float() makes of it (py::float_), which
// is also what the checks above see: an offset of Fraction(1, 10**400) is 0.0
// (no shift). _check_knobs checks and returns the same doubles.
// The result is a Knobs (level_guard.hpp), which restate_knobs applies.

namespace detail {

inline std::string quoted(const std::string& shown) {
    return py::repr(py::str(shown)).cast<std::string>();
}

inline std::string repr_of(const py::handle& value) {
    return py::repr(value).cast<std::string>();
}

// numbers.Real minus bool: what _check_knobs accepts as a weight/exponent.
// numbers.Real is looked up once per module (gil_safe_call_once_and_store,
// which also keeps it alive until the interpreter is gone), not per knob.
inline bool is_real_number(const py::handle& value) {
    if (py::isinstance<py::bool_>(value)) return false;
    PYBIND11_CONSTINIT static py::gil_safe_call_once_and_store<py::object> numbers_real;
    const py::object& real =
            numbers_real
                    .call_once_and_store_result(
                            [] { return py::module_::import("numbers").attr("Real"); })
                    .get_stored();
    return py::isinstance(value, real);
}

// bool or numpy.bool_ (the only bool-like values clip accepts).
inline bool is_bool(const py::handle& value) {
    if (py::isinstance<py::bool_>(value)) return true;
    const py::handle type = py::type::handle_of(value);
    const std::string module = py::str(type.attr("__module__")).cast<std::string>();
    const std::string qualname = py::str(type.attr("__qualname__")).cast<std::string>();
    return module == "numpy" && (qualname == "bool_" || qualname == "bool");
}

}  // namespace detail

// Reads and checks one pick's knobs; `shown` is the Python name used in the
// messages.
inline Knobs read_knobs(const py::handle& item, const std::string& shown) {
    Knobs knobs;
    const std::string label = "constraint " + detail::quoted(shown);
    const py::object weight = item.attr("weight");
    if (!weight.is_none()) {
        if (!detail::is_real_number(weight)) {
            throw py::type_error(label + ": weight must be a number or None, not " +
                                 detail::repr_of(weight));
        }
        const double value = py::float_(weight).cast<double>();
        if (!std::isfinite(value) || value < 0.0) {
            throw py::value_error(label + ": weight must be finite and >= 0, not " +
                                  detail::repr_of(weight));
        }
        knobs.weight = value;
    }
    const py::object level = item.attr("level");
    if (!level.is_none()) {
        if (!py::isinstance<py::str>(level)) {
            throw py::type_error(label + ": level must be a Level, its name or None, not " +
                                 detail::repr_of(level));
        }
        knobs.level = level_named(level.cast<std::string>());
        if (!knobs.level) {
            throw py::value_error(label + ": level must be 'hard', 'medium' or 'soft', not " +
                                  detail::repr_of(level));
        }
    }
    const py::object exponent = item.attr("exponent");
    if (!exponent.is_none()) {
        if (!detail::is_real_number(exponent)) {
            throw py::type_error(label + ": exponent must be a number, not " +
                                 detail::repr_of(exponent));
        }
        const double value = py::float_(exponent).cast<double>();
        if (!std::isfinite(value) || value <= 0.0) {
            throw py::value_error(label + ": exponent must be finite and > 0, not " +
                                  detail::repr_of(exponent));
        }
        knobs.exponent = value;
    }
    const py::object offset = py::getattr(item, "offset", py::none());
    if (!offset.is_none()) {
        if (!detail::is_real_number(offset)) {
            throw py::type_error(label + ": offset must be a number, not " +
                                 detail::repr_of(offset));
        }
        const double value = py::float_(offset).cast<double>();
        if (!std::isfinite(value)) {
            throw py::value_error(label + ": offset must be finite, not " +
                                  detail::repr_of(offset));
        }
        knobs.offset = value;
    }
    const py::object clip = py::getattr(item, "clip", py::none());
    if (!clip.is_none()) {
        if (!detail::is_bool(clip)) {
            throw py::type_error(label + ": clip must be True or False, not " +
                                 detail::repr_of(clip));
        }
        knobs.clip = PyObject_IsTrue(clip.ptr()) == 1;
    }
    return knobs;
}

// Re-states one pick's knobs on a component a domain factory built (with
// its kDefaultWeight / kDefaultLevel, so None keeps them): the same checks
// and the same rule (restate_knobs, level_guard.hpp; the exponent is always
// applied) as apply_constraints, for code that cannot go through an assembly
// (employee-scheduling's rule factory, which may add a rule several times).
// Apply it to the OUTERMOST component — after wrapping in a ScopedUnit, which
// copies the inner component's metadata.
//
//   auto unit = std::make_unique<MaxConsecutiveDaysCost>(
//           MaxConsecutiveDaysCost::kDefaultWeight, value);
//   python::apply_knobs(*unit, pick, "max_consecutive_days_cost", MediumFold::none());
inline void apply_knobs(ConstraintMetadata& constraint, const py::handle& item,
                       const std::string& shown, MediumFold fold) {
    const Knobs knobs = read_knobs(item, shown);
    try {
        restate_knobs(constraint, knobs, fold, detail::quoted(shown));
    } catch (const std::invalid_argument& error) {
        throw py::value_error(error.what());
    }
}

// Applies the constraint picks coming from Python (<pkg>.constraints) to a
// domain's default assembly: the name of a component plus the metadata to
// re-state on it. Duck-typed on purpose — any object carrying
// name/weight/level/exponent (and optionally offset/clip) works, which is what
// lets the spec dataclasses stay plain Python. The five knobs:
//   weight    None = the component's kDefaultWeight (in units of its level),
//             else a finite number >= 0
//   level     None = the component's kDefaultLevel, otherwise "hard",
//             "medium" or "soft" (a Python Level). See checked_level for what
//             a domain or a constraint refuses.
//   exponent  1.0 = no shaping, else finite and > 0; always applied, so an
//             explicit 1.0 (or None) resets a default assembly component
//             built with another exponent, as the Python score() reads it
//   offset    0.0 = no shift, else finite (any sign); always applied
//   clip      True = the threshold, False = the literal formula (shape_cost);
//             always applied. The hard-0 rule: check_literal_offset.
// (read_knobs above). A wrong type is a TypeError, every other refusal a
// ValueError, each naming the constraint by its Python name, in the words
// of the Python base.
//
// The picks REPLACE the default set: passing three of them scores exactly
// those three. Passing None keeps every default; an empty list is an error,
// not "the defaults" (a filter that came back empty is a mistake, and the
// Python get_constraints() rejects it for the same reason). Since the
// assembly already looks components up by name, there is no name -> factory
// registry here to drift out of sync.
//
// A name may be picked several times. Every pick is its own instance with
// its own knobs, scored independently; the instances add up (identical
// picks simply count twice). The first pick of a name re-states the default
// component; each further pick is a fresh copy of it, taken from a new
// default assembly (so the same domain parameters as the default instance),
// with that pick's knobs, appended after it (ConstraintAssembly::
// add_instances). constraint_costs() keys the k-th instance "<name>#<k>"
// (instance_keys), the order Python's instance_keys derives from the list.
//
//   tfpy::apply_constraints([] { return defaultConstraints(); }, constraints)
//   tfpy::apply_constraints([&] { return defaultConstraints(problem); }, constraints)
//
// `defaults` is a callable returning the domain's default assembly; it is
// called once, plus once per repeated pick. A pick's domain parameters
// (params()) are not read here: a domain whose constraints take parameters
// builds each pick's component itself and re-states its knobs with
// apply_knobs (employee-scheduling's rule factory).
template <typename Factory,
          typename = std::enable_if_t<std::is_invocable_v<Factory&>>>
std::decay_t<std::invoke_result_t<Factory&>> apply_constraints(Factory&& defaults,
                                                              const py::object& constraints) {
    using Assembly = std::decay_t<std::invoke_result_t<Factory&>>;
    Assembly assembly = defaults();
    if (constraints.is_none()) return assembly;
    std::vector<std::string> picked;  // each name once, in first-pick order
    std::vector<Assembly> copies;     // one single-component assembly per repeat
    for (py::handle item : constraints) {
        const std::string name = resolve_name(assembly, item.attr("name").template cast<std::string>());
        const std::string shown = python_name(name);
        const Knobs knobs = read_knobs(item, shown);
        const bool repeat = std::find(picked.begin(), picked.end(), name) != picked.end();
        try {
            if (!repeat) {
                restate_knobs(assembly, name, knobs, detail::quoted(shown));
            } else {
                Assembly copy = defaults();
                restate_knobs(copy, name, knobs, detail::quoted(shown));
                copy.only(std::vector<std::string>{name});
                copies.push_back(std::move(copy));
            }
        } catch (const std::invalid_argument& error) {
            throw py::value_error(error.what());
        }
        if (!repeat) picked.push_back(name);
    }
    if (picked.empty()) {
        throw py::value_error("constraints must not be empty (pass None for the defaults)");
    }
    assembly.only(picked);
    for (Assembly& copy : copies) assembly.add_instances(std::move(copy));
    return assembly;
}

// Rule 1 of "The range of a score" (level.hpp) in Python's words: the
// constraint's Python name and Python's repr() of the contribution, so a
// solve and score(constraint_costs(...), picks) refuse the same pick with
// the same OverflowError text. The sign rule (SignedCostLevelError, thrown at
// reset) likewise becomes the mirror's ValueError. Module-local: each
// package's extension translates the exceptions its own copy of the engine
// throws.
inline void register_contribution_overflow_translator() {
    py::register_local_exception_translator([](std::exception_ptr pending) {
        try {
            if (pending) std::rethrow_exception(pending);
        } catch (const ContributionOverflowError& error) {
            const std::string shown = py::repr(py::float_(error.value())).cast<std::string>();
            const std::string text = ContributionOverflowError::message(
                    python_name(error.constraint()), shown, error.beyond_hard_limit());
            PyErr_SetString(PyExc_OverflowError, text.c_str());
        } catch (const SignedCostLevelError& error) {
            const std::string text =
                    SignedCostLevelError::message(python_name(error.constraint()), error.level());
            PyErr_SetString(PyExc_ValueError, text.c_str());
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
//   bind_constraint_info(m, default_assembly)
//       one row per constraint of the DEFAULT assembly, in list_constraints()
//       order; default_enabled = default_weight > 0.
//   bind_constraint_info(m, default_assembly, catalog_assembly)
//       for a domain that builds more constraints than it scores by default
//       (employee-scheduling): catalog_info() — the default rows, then one row
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
inline int& failing_worker_thread() {
    static int index = -1;
    return index;
}

inline void fail_worker_thread_start(int index) {
    if (index == failing_worker_thread()) {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                "injected worker thread creation failure");
    }
}

// `<pkg>._core._fail_worker_thread_start(index)`: a private test seam over
// optimaru::worker_thread_start_hook (impl/solver/worker_threads.hpp). From the
// next parallel solve on, creating worker thread `index` of every pool in this
// module fails with std::system_error ("injected worker thread creation
// failure", a RuntimeError in Python), as std::thread does when the system
// has no thread left; -1 switches it off again. The package tests use it to
// check that such a solve stops and joins the workers already running and
// raises, where it used to call std::terminate. Not for production use.
inline void bind_worker_thread_seam(py::module_& m) {
    m.def(
            "_fail_worker_thread_start",
            [](int index) {
                failing_worker_thread() = index;
                worker_thread_start_hook = index < 0 ? nullptr : &fail_worker_thread_start;
            },
            py::arg("index"),
            "Test seam: fail the creation of worker thread `index` in every parallel "
            "solve from now on (-1 = off).");
}

template <typename Rows>
void bind_info_rows(py::module_& m, Rows rows_of) {
    register_contribution_overflow_translator();
    bind_worker_thread_seam(m);
    py::object namedtuple = py::module_::import("collections").attr("namedtuple");
    py::object row_type = namedtuple(
            "ConstraintInfo",
            py::make_tuple("name", "default_level", "default_weight", "level_flippable", "reward",
                           "default_enabled"));
    row_type.attr("__module__") = m.attr("__name__");
    row_type.attr("__doc__") =
            "One catalog row: the constraint's Python name, its default level "
            "('hard' | 'medium' | 'soft'), its default weight (in units of that "
            "level), whether a pick's level may change it, whether it is a "
            "reward (raw cost <= 0; level='hard' is refused), and whether it is "
            "scored when no constraints are named (in the default set at a "
            "default weight > 0).";
    m.attr("ConstraintInfo") = row_type;
    m.def(
            "constraint_info",
            [row_type, rows_of]() {
                py::list rows;
                for (const ConstraintInfo& row : rows_of()) {
                    rows.append(row_type(python_name(row.name), level_name(row.default_level),
                                        row.default_weight, row.level_flippable, row.reward,
                                        row.default_enabled));
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
void bind_constraint_info(py::module_& m, Factory default_assembly) {
    detail::bind_info_rows(m, [default_assembly]() { return default_assembly().info(); });
}

template <typename DefaultFactory, typename CatalogFactory>
void bind_constraint_info(py::module_& m, DefaultFactory default_assembly,
                        CatalogFactory catalog_assembly) {
    // catalog_info's std::logic_error (a default declared differently in the
    // two assemblies: a domain bug) reaches Python as RuntimeError.
    detail::bind_info_rows(m, [default_assembly, catalog_assembly]() {
        return catalog_info(default_assembly(), catalog_assembly());
    });
}

}  // namespace python
}  // namespace optimaru

#endif  // OPTIMARU_PYTHON_CONSTRAINT_BINDING_HPP

// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_CONSTRAINT_ASSEMBLY_HPP
#define OPTIMARU_CONSTRAINT_ASSEMBLY_HPP

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "optimaru/decomposed_objective.hpp"

namespace optimaru {

// One row of a domain's constraint catalog: the component's registry name and
// the defaults it was constructed with. What `<pkg>.constraint_info()`
// reports (python/constraint_binding.hpp). Read off the DEFAULT assembly
// (ConstraintAssembly::info()), or off the default assembly plus a catalog
// assembly for a domain that can build more constraints than it scores by
// default (catalog_info() below).
struct ConstraintInfo {
    std::string name;      // the registry name ("facilityCapacityCost")
    Level default_level;
    double default_weight;  // in units of default_level
    bool level_flippable;
    bool reward;
    bool is_global;
    // Scored when the caller names no constraints: in the default assembly
    // AND at a default weight > 0. False for an opt-in objective whose
    // default weight is 0 (vehicle-routing EnclosingCircleRadiusCost) and for
    // a constraint only a catalog assembly holds (employee-scheduling
    // MaxConsecutiveDaysCost). The server catalog's default_enabled.
    bool default_enabled = true;
};

// ---------------------------------------------------------------------------
// ConstraintAssembly
// ---------------------------------------------------------------------------
// A declarative way to state a cost function: start from a domain's default
// assembly, then say only what differs from it — by constraint NAME, chained,
// in one expression.
//
//   DecomposedObjective calc;
//   defaultConstraints()                         // every default already in
//       .weight("facilitySetupCost", 4)          // ... only what you care about
//       .soft("facilityCapacityCost")
//       .drop("distanceFromFacilityCost")
//       .into(calc);
//   solver.solve(problem, calc);
//
// Without it the same edit means reaching for the components by type, which
// costs three lines and a std::move just to flip one level:
//
//   auto component = std::make_unique<FacilityCapacityCost>();
//   component->level = optimaru::Level::Soft;
//   calc.add(std::move(component));
//   calc.emplace<FacilitySetupCost>(4.0);
//   ...and re-listing every constraint you did NOT want to change.
//
// Names are the ones the components carry (UnitConstraint::name /
// GlobalConstraint::name) — the same strings constraint_totals() reports, so
// what you read in a score breakdown is what you type here. Every lookup that
// misses THROWS with the available names listed: a typo must not silently
// leave the cost function unchanged.
//
// Assemblies are ordinary movable values, so a domain hands one back from a
// factory and the caller adjusts it. into() moves the components into the
// objective and leaves the assembly empty — build one, spend it once.
//
// One name, several instances: a constraint stated with several sets of knobs
// (x^2 + x^3 as exponent 2 plus exponent 3, a capped and an uncapped travel
// time) is the same component added more than once. add() keeps its
// override semantics; add_instance() / add_instances() append further
// instances next to the one already held, each scored on its own with its
// own metadata, and the instances add up. Every by-name edit and lookup
// (weight(), level(), hard(), soft(), exponent(), offset(), clip(), find(),
// add()'s replacement) addresses the FIRST instance of the name; drop()
// removes the first, only() keeps every instance of the names it is given.
// The objective, and the Python constraint_costs() keys
// (python::instance_keys: "<name>", "<name>#2", ...), see the instances of a
// name in the order they were added. Those keys are why a name must not
// contain '#': every add / add_instance / emplace refuses one
// (std::invalid_argument naming the constraint).

template <typename Solution>
class ConstraintAssembly {
public:
    using Unit = UnitConstraint<Solution>;
    using Global = GlobalConstraint<Solution>;

    // --- building ---------------------------------------------------------
    //
    // Every builder method comes in two forms so both spellings work:
    //   defaults().weight(...).into(calc);   // chain a temporary, spend it
    //   auto a = defaults().weight(...);     // ... or keep it in a variable
    // The rvalue form returns an rvalue reference, so binding the result MOVES
    // the assembly instead of copying it (its components are move-only).

    // Adds a constraint, REPLACING any already in the assembly under the same
    // name (override semantics: stating a constraint twice means "this one",
    // not "count it twice"). Use add_instance() below when you really do want
    // the same cost counted more than once (each instance with its own knobs).
    ConstraintAssembly& add(std::unique_ptr<Unit> constraint) & {
        replace_or_append(units_, checked_name(std::move(constraint)));
        return *this;
    }
    ConstraintAssembly&& add(std::unique_ptr<Unit> constraint) && {
        add(std::move(constraint));
        return std::move(*this);
    }
    ConstraintAssembly& add(std::unique_ptr<Global> constraint) & {
        replace_or_append(globals_, checked_name(std::move(constraint)));
        return *this;
    }
    ConstraintAssembly&& add(std::unique_ptr<Global> constraint) && {
        add(std::move(constraint));
        return std::move(*this);
    }
    template <typename C, typename... Args>
    ConstraintAssembly& emplace(Args&&... args) & {
        return add(std::make_unique<C>(std::forward<Args>(args)...));
    }
    template <typename C, typename... Args>
    ConstraintAssembly&& emplace(Args&&... args) && {
        add(std::make_unique<C>(std::forward<Args>(args)...));
        return std::move(*this);
    }

    // Appends another instance of a constraint, KEEPING any already held
    // under the same name (see "One name, several instances" above).
    ConstraintAssembly& add_instance(std::unique_ptr<Unit> constraint) & {
        units_.push_back(checked_name(std::move(constraint)));
        return *this;
    }
    ConstraintAssembly&& add_instance(std::unique_ptr<Unit> constraint) && {
        add_instance(std::move(constraint));
        return std::move(*this);
    }
    ConstraintAssembly& add_instance(std::unique_ptr<Global> constraint) & {
        globals_.push_back(checked_name(std::move(constraint)));
        return *this;
    }
    ConstraintAssembly&& add_instance(std::unique_ptr<Global> constraint) && {
        add_instance(std::move(constraint));
        return std::move(*this);
    }
    template <typename C, typename... Args>
    ConstraintAssembly& emplace_instance(Args&&... args) & {
        return add_instance(std::make_unique<C>(std::forward<Args>(args)...));
    }
    template <typename C, typename... Args>
    ConstraintAssembly&& emplace_instance(Args&&... args) && {
        add_instance(std::make_unique<C>(std::forward<Args>(args)...));
        return std::move(*this);
    }
    // Appends every component of `other` as further instances (units after
    // this assembly's units, globals after its globals, in `other`'s order)
    // and leaves `other` empty. How a copy of a default component joins the
    // assembly: build the domain's default assembly again, re-state the
    // knobs on it, keep only the one name, and append it.
    ConstraintAssembly& add_instances(ConstraintAssembly&& other) & {
        for (auto& constraint : other.units_) units_.push_back(std::move(constraint));
        for (auto& constraint : other.globals_) globals_.push_back(std::move(constraint));
        other.units_.clear();
        other.globals_.clear();
        return *this;
    }
    ConstraintAssembly&& add_instances(ConstraintAssembly&& other) && {
        add_instances(std::move(other));
        return std::move(*this);
    }

    // --- adjusting --------------------------------------------------------
    // The level, the weight and the exponent are assembly-time metadata, not
    // part of any cost, which is exactly why they can be re-stated here.
    // Every setter validates what it is given and throws std::invalid_argument
    // (the Python bindings surface it as ValueError); the rules live in
    // ConstraintMetadata::set_weight/set_level/set_exponent:
    //   weight    finite and >= 0, in units of the constraint's level
    //   exponent  finite and > 0
    //   offset    finite (any sign)
    //   clip      any bool
    //   level     a reward can never be Hard; a constraint whose
    //             level_flippable is false keeps its level (re-stating the
    //             level it already has is accepted as a no-op)
    // The rule that spans several knobs (the hard-0 rule, python::
    // check_literal_offset) is checked at reset (validate_level), so the
    // setters may come in any order.

    ConstraintAssembly& weight(const std::string& name, double weight) & {
        require(name).set_weight(weight);
        return *this;
    }
    ConstraintAssembly&& weight(const std::string& name, double weight) && {
        this->weight(name, weight);  // the parameter shadows the method name
        return std::move(*this);
    }
    // Any level, Medium included (the domain must declare a medium fold, or
    // the objective throws at reset; Python picks are refused earlier, by
    // python::checked_level).
    ConstraintAssembly& level(const std::string& name, Level level) & {
        require(name).set_level(level);
        return *this;
    }
    ConstraintAssembly&& level(const std::string& name, Level level) && {
        this->level(name, level);  // the parameter shadows the method
        return std::move(*this);
    }
    // hard(name) == level(name, Level::Hard): throws for a reward and for a
    // non-flippable soft constraint. A medium constraint becomes hard at the
    // same weight (one hard unit per raw unit).
    ConstraintAssembly& hard(const std::string& name) & { return level(name, Level::Hard); }
    ConstraintAssembly&& hard(const std::string& name) && {
        hard(name);
        return std::move(*this);
    }
    // soft(name) == level(name, Level::Soft): demotes a hard or a medium
    // constraint; throws for a non-flippable hard one.
    ConstraintAssembly& soft(const std::string& name) & { return level(name, Level::Soft); }
    ConstraintAssembly&& soft(const std::string& name) && {
        soft(name);
        return std::move(*this);
    }
    // Sign-preserving power shaping: per unit before the weight for a unit
    // constraint, on the single total for a global one. 2 makes concentrated
    // violations disproportionately expensive.
    ConstraintAssembly& exponent(const std::string& name, double exponent) & {
        require(name).set_exponent(exponent);
        return *this;
    }
    ConstraintAssembly&& exponent(const std::string& name, double exponent) && {
        this->exponent(name, exponent);  // the parameter shadows the method
        return std::move(*this);
    }
    // The offset and clip knobs of the shaping: see shape_cost
    // (decomposed_objective.hpp) for the formula.
    ConstraintAssembly& offset(const std::string& name, double offset) & {
        require(name).set_offset(offset);
        return *this;
    }
    ConstraintAssembly&& offset(const std::string& name, double offset) && {
        this->offset(name, offset);  // the parameter shadows the method
        return std::move(*this);
    }
    ConstraintAssembly& clip(const std::string& name, bool clip) & {
        require(name).set_clip(clip);
        return *this;
    }
    ConstraintAssembly&& clip(const std::string& name, bool clip) && {
        this->clip(name, clip);  // the parameter shadows the method
        return std::move(*this);
    }

    // Removes a constraint. Throws when it is not in the assembly, so dropping
    // something twice (or misspelling it) is an error rather than a silent
    // no-op.
    ConstraintAssembly& drop(const std::string& name) & {
        if (!erase_by_name(units_, name) && !erase_by_name(globals_, name)) {
            throw std::invalid_argument("Constraint '" + name +
                                        "' is not in this assembly; it holds " + name_list());
        }
        return *this;
    }
    ConstraintAssembly&& drop(const std::string& name) && {
        drop(name);
        return std::move(*this);
    }

    // Keeps ONLY the named constraints, dropping the rest — the short way to
    // isolate a few rules ("score just the hard ones"). Throws on any name the
    // assembly does not hold.
    ConstraintAssembly& only(std::initializer_list<std::string> names) & {
        only_impl(names);
        return *this;
    }
    ConstraintAssembly&& only(std::initializer_list<std::string> names) && {
        only(names);
        return std::move(*this);
    }
    // Same, for a list decided at runtime — what a caller crossing a language
    // boundary has (the Python packages hand their constraint picks in as a
    // vector of names).
    ConstraintAssembly& only(const std::vector<std::string>& names) & {
        only_impl(names);
        return *this;
    }
    ConstraintAssembly&& only(const std::vector<std::string>& names) && {
        only(names);
        return std::move(*this);
    }

    // --- inspecting -------------------------------------------------------

    bool contains(const std::string& name) const {
        return find_in(units_, name) != nullptr || find_in(globals_, name) != nullptr;
    }
    std::vector<std::string> names() const {
        std::vector<std::string> all;
        all.reserve(units_.size() + globals_.size());
        for (const auto& constraint : units_) all.push_back(constraint->name);
        for (const auto& constraint : globals_) all.push_back(constraint->name);
        return all;
    }
    std::size_t size() const { return units_.size() + globals_.size(); }
    bool empty() const { return size() == 0; }

    // The metadata of one component, or nullptr. Read-only: edits go through
    // the validating setters above.
    const ConstraintMetadata* find(const std::string& name) const {
        if (const Unit* unit = find_in(units_, name)) return unit;
        if (const Global* global = find_in(globals_, name)) return global;
        return nullptr;
    }

    // The catalog rows of this assembly, in assembly order (units first, then
    // globals, like names()). Read off a domain's DEFAULT assembly this is its
    // constraint catalog, so every default lives in exactly one place: the
    // component's constructor (kDefaultWeight / kDefaultLevel).
    // default_enabled = weight > 0 (an assembly holds what it scores).
    std::vector<ConstraintInfo> info() const {
        std::vector<ConstraintInfo> rows;
        rows.reserve(size());
        for (const auto& c : units_) {
            rows.push_back({c->name, c->level, c->weight, c->level_flippable, c->reward, false,
                            c->weight > 0.0});
        }
        for (const auto& c : globals_) {
            rows.push_back({c->name, c->level, c->weight, c->level_flippable, c->reward, true,
                            c->weight > 0.0});
        }
        return rows;
    }

    // --- spending ---------------------------------------------------------

    // Moves the components into `objective` and leaves this assembly empty.
    void into(DecomposedObjective<Solution>& objective) {
        for (auto& constraint : units_) objective.add(std::move(constraint));
        for (auto& constraint : globals_) objective.add(std::move(constraint));
        units_.clear();
        globals_.clear();
    }

private:
    // '#' separates a name from its instance number in the constraint_costs()
    // keys ("<name>#<k>"), so a name holding one could collide with another
    // name's repeat.
    template <typename Pointer>
    static Pointer checked_name(Pointer constraint) {
        if (constraint->name.find('#') != std::string::npos) {
            throw std::invalid_argument("Constraint '" + constraint->name +
                                        "': a name must not contain '#', which keys repeated "
                                        "instances (\"<name>#<k>\")");
        }
        return constraint;
    }

    template <typename Vector, typename Pointer>
    static void replace_or_append(Vector& vector, Pointer constraint) {
        for (auto& existing : vector) {
            if (existing->name == constraint->name) {
                existing = std::move(constraint);
                return;
            }
        }
        vector.push_back(std::move(constraint));
    }

    template <typename Vector>
    static auto find_in(const Vector& vector, const std::string& name) ->
            typename Vector::value_type::element_type* {
        for (const auto& constraint : vector) {
            if (constraint->name == name) return constraint.get();
        }
        return nullptr;
    }

    template <typename Vector>
    static bool erase_by_name(Vector& vector, const std::string& name) {
        for (auto it = vector.begin(); it != vector.end(); ++it) {
            if ((*it)->name == name) {
                vector.erase(it);
                return true;
            }
        }
        return false;
    }

    template <typename Vector, typename Names>
    static void keep_only(Vector& vector, const Names& names) {
        Vector kept;
        for (auto& constraint : vector) {
            for (const std::string& name : names) {
                if (constraint->name == name) {
                    kept.push_back(std::move(constraint));
                    break;
                }
            }
        }
        vector = std::move(kept);
    }

    template <typename Names>
    void only_impl(const Names& names) {
        for (const std::string& name : names) {
            if (!contains(name)) {
                throw std::invalid_argument("Constraint '" + name +
                                            "' is not in this assembly; it holds " + name_list());
            }
        }
        keep_only(units_, names);
        keep_only(globals_, names);
    }

    // The named constraint's metadata, whichever kind it is (both bases share
    // ConstraintMetadata). Throws with the available names when it misses.
    ConstraintMetadata& require(const std::string& name) {
        if (Unit* unit = find_in(units_, name)) return *unit;
        if (Global* global = find_in(globals_, name)) return *global;
        throw std::invalid_argument("Constraint '" + name + "' is not in this assembly; it holds " +
                                    name_list());
    }

    std::string name_list() const {
        std::string list;
        for (const std::string& name : names()) {
            if (!list.empty()) list += ", ";
            list += "'" + name + "'";
        }
        return list.empty() ? "no constraints" : list;
    }

    std::vector<std::unique_ptr<Unit>> units_;
    std::vector<std::unique_ptr<Global>> globals_;
};

// ---------------------------------------------------------------------------
// catalog_info: the catalog of a domain that builds more than its defaults
// ---------------------------------------------------------------------------
// Most domains score every constraint they have by default, so their catalog
// is `defaultConstraints().info()`. A domain whose factory can build
// constraints that are NOT in the default set (employee-scheduling: eight
// defaults, about thirty buildable rules) keeps a second assembly,
// `catalogConstraints()`, holding one instance of every buildable constraint
// at its default parameters and weight, and reports
//
//   catalog_info(defaultConstraints(), catalogConstraints())
//
// = the default rows first (defaults order, default_enabled = weight > 0),
// then each catalog row whose name is not a default (catalog order,
// default_enabled = false). So the catalog starts with list_constraints() and
// every name the factory accepts has exactly one row. A name in both
// assemblies must agree on its level, weight, flippability, reward and kind,
// or this throws std::logic_error: two declarations of one default would be
// the drift the catalog exists to prevent.
template <typename Solution>
std::vector<ConstraintInfo> catalog_info(const ConstraintAssembly<Solution>& defaults,
                                        const ConstraintAssembly<Solution>& catalog) {
    std::vector<ConstraintInfo> rows = defaults.info();
    const std::size_t default_count = rows.size();
    for (ConstraintInfo& row : catalog.info()) {
        bool known = false;
        for (std::size_t i = 0; i < default_count; ++i) {
            const ConstraintInfo& d = rows[i];
            if (d.name != row.name) continue;
            known = true;
            if (d.default_level != row.default_level || d.default_weight != row.default_weight ||
                d.level_flippable != row.level_flippable || d.reward != row.reward ||
                d.is_global != row.is_global) {
                throw std::logic_error("Constraint '" + row.name +
                                       "' is declared differently in the default and the "
                                       "catalog assembly");
            }
        }
        if (known) continue;
        row.default_enabled = false;
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // namespace optimaru

#endif  // OPTIMARU_CONSTRAINT_ASSEMBLY_HPP

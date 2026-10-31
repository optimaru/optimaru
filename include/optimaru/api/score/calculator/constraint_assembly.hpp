#ifndef OPTIMARU_API_SCORE_CALCULATOR_CONSTRAINT_ASSEMBLY_HPP
#define OPTIMARU_API_SCORE_CALCULATOR_CONSTRAINT_ASSEMBLY_HPP

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// ConstraintAssembly
// ---------------------------------------------------------------------------
// A declarative way to state a cost function: start from a domain's default
// assembly, then say only what differs from it — by constraint NAME, chained,
// in one expression.
//
//   DecomposedCalculator calc;
//   defaultConstraints()                     // every default already in
//       .weight("facility setup cost", 4)    // ... only what you care about
//       .soft("facility capacity")
//       .drop("distance from facility")
//       .into(calc);
//   solver.solve(problem, calc);
//
// Without it the same edit means reaching for the components by type, which
// costs three lines and a std::move just to flip one level:
//
//   auto component = std::make_unique<FacilityCapacity>();
//   component->isHard = false;
//   calc.add(std::move(component));
//   calc.emplace<FacilitySetupCost>(4.0);
//   ...and re-listing every constraint you did NOT want to change.
//
// Names are the ones the components carry (UnitConstraint::name /
// GlobalConstraint::name) — the same strings constraintTotals() reports, so
// what you read in a score breakdown is what you type here. Every lookup that
// misses THROWS with the available names listed: a typo must not silently
// leave the cost function unchanged.
//
// Assemblies are ordinary movable values, so a domain hands one back from a
// factory and the caller adjusts it. into() moves the components into the
// calculator and leaves the assembly empty — build one, spend it once.

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
    // not "count it twice"). Use the calculator's own add() when you really do
    // want the same cost counted more than once.
    ConstraintAssembly& add(std::unique_ptr<Unit> constraint) & {
        replaceOrAppend(units_, std::move(constraint));
        return *this;
    }
    ConstraintAssembly&& add(std::unique_ptr<Unit> constraint) && {
        add(std::move(constraint));
        return std::move(*this);
    }
    ConstraintAssembly& add(std::unique_ptr<Global> constraint) & {
        replaceOrAppend(globals_, std::move(constraint));
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

    // --- adjusting --------------------------------------------------------
    // The level, the weight and the exponent are assembly-time metadata, not
    // part of any cost, which is exactly why they can be re-stated here.

    ConstraintAssembly& weight(const std::string& name, double weight) & {
        require(name, [weight](auto& constraint) { constraint.weight = weight; });
        return *this;
    }
    ConstraintAssembly&& weight(const std::string& name, double weight) && {
        this->weight(name, weight);  // the parameter shadows the method name
        return std::move(*this);
    }
    ConstraintAssembly& hard(const std::string& name) & {
        require(name, [](auto& constraint) { constraint.isHard = true; });
        return *this;
    }
    ConstraintAssembly&& hard(const std::string& name) && {
        hard(name);
        return std::move(*this);
    }
    ConstraintAssembly& soft(const std::string& name) & {
        require(name, [](auto& constraint) { constraint.isHard = false; });
        return *this;
    }
    ConstraintAssembly&& soft(const std::string& name) && {
        soft(name);
        return std::move(*this);
    }
    // Sign-preserving power shaping applied per unit before the weight; 2 makes
    // concentrated violations disproportionately expensive.
    ConstraintAssembly& exponent(const std::string& name, double exponent) & {
        require(name, [exponent](auto& constraint) { constraint.exponent = exponent; });
        return *this;
    }
    ConstraintAssembly&& exponent(const std::string& name, double exponent) && {
        this->exponent(name, exponent);  // the parameter shadows the method
        return std::move(*this);
    }

    // Removes a constraint. Throws when it is not in the assembly, so dropping
    // something twice (or misspelling it) is an error rather than a silent
    // no-op.
    ConstraintAssembly& drop(const std::string& name) & {
        if (!eraseByName(units_, name) && !eraseByName(globals_, name)) {
            throw std::invalid_argument("Constraint '" + name +
                                        "' is not in this assembly; it holds " + nameList());
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
        onlyImpl(names);
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
        onlyImpl(names);
        return *this;
    }
    ConstraintAssembly&& only(const std::vector<std::string>& names) && {
        only(names);
        return std::move(*this);
    }

    // --- inspecting -------------------------------------------------------

    bool contains(const std::string& name) const {
        return findIn(units_, name) != nullptr || findIn(globals_, name) != nullptr;
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

    // --- spending ---------------------------------------------------------

    // Moves the components into `calculator` and leaves this assembly empty.
    void into(DecomposedScoreCalculator<Solution>& calculator) {
        for (auto& constraint : units_) calculator.add(std::move(constraint));
        for (auto& constraint : globals_) calculator.add(std::move(constraint));
        units_.clear();
        globals_.clear();
    }

private:
    template <typename Vector, typename Pointer>
    static void replaceOrAppend(Vector& vector, Pointer constraint) {
        for (auto& existing : vector) {
            if (existing->name == constraint->name) {
                existing = std::move(constraint);
                return;
            }
        }
        vector.push_back(std::move(constraint));
    }

    template <typename Vector>
    static auto findIn(const Vector& vector, const std::string& name) ->
            typename Vector::value_type::element_type* {
        for (const auto& constraint : vector) {
            if (constraint->name == name) return constraint.get();
        }
        return nullptr;
    }

    template <typename Vector>
    static bool eraseByName(Vector& vector, const std::string& name) {
        for (auto it = vector.begin(); it != vector.end(); ++it) {
            if ((*it)->name == name) {
                vector.erase(it);
                return true;
            }
        }
        return false;
    }

    template <typename Vector, typename Names>
    static void keepOnly(Vector& vector, const Names& names) {
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
    void onlyImpl(const Names& names) {
        for (const std::string& name : names) {
            if (!contains(name)) {
                throw std::invalid_argument("Constraint '" + name +
                                            "' is not in this assembly; it holds " + nameList());
            }
        }
        keepOnly(units_, names);
        keepOnly(globals_, names);
    }

    // Applies `apply` to the named constraint, whichever kind it is. The two
    // constraint types share the metadata field names, so one generic lambda
    // covers both.
    template <typename F>
    void require(const std::string& name, F&& apply) {
        if (Unit* unit = findIn(units_, name)) {
            apply(*unit);
            return;
        }
        if (Global* global = findIn(globals_, name)) {
            apply(*global);
            return;
        }
        throw std::invalid_argument("Constraint '" + name + "' is not in this assembly; it holds " +
                                    nameList());
    }

    std::string nameList() const {
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

}  // namespace optimaru

#endif  // OPTIMARU_API_SCORE_CALCULATOR_CONSTRAINT_ASSEMBLY_HPP

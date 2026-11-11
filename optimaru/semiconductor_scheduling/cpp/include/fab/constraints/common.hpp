#ifndef FAB_CONSTRAINTS_COMMON_HPP
#define FAB_CONSTRAINTS_COMMON_HPP

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "fab/domain.hpp"
#include "fab/evaluator.hpp"
#include "fab/score_terms.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace fab {

// ---------------------------------------------------------------------------
// Plugin-style constraint components
// ---------------------------------------------------------------------------
// The 20 constraints of the fab, one component each on the engine's
// DecomposedObjective — the composable path NEXT TO the monolithic
// calculators (fab/score_calculator.hpp, fab/incremental_calculator.hpp),
// which stay the default fast path.
//
// Unit choice: none. The decision value of a row is a (rank, machine option)
// pair and the schedule is DERIVED from all of them by the serial decoder:
// changing one row's rank can move every later operation on every machine.
// No cost decomposes per decision value, so all 20 components are
// GlobalConstraints over one shared ScheduleView (the engine's
// shared-context contract, the conference-scheduling BreakdownProjection
// precedent): the view keeps the decoded schedule and the per-unit raw costs
// up to date — lazily, once per score, however many components read it (see
// ScheduleEvaluator) — and each component reports its own term.
//
// Each component still owns its cost: the raw formula over one unit (a row, a
// lot, a machine, a resource, a pin or a machine group) is a static function
// of its struct, which the view and the monolithic calculators call. A
// formula is therefore written once, in the component's header.
//
// The conventions every use case shares are described once: editing an
// assembly by name in optimaru/score/constraint_assembly.hpp; kDefaultWeight /
// kDefaultLevel, the registry names and the cost sign (positive = penalty, 0 =
// no violation) in docs/extending.md. The schedule carries no weights. Every
// component requires ScoreCalculator::prepare(schedule) to have run.
//
// Global constraints shape their TOTAL: exponent e scores weight * total^e
// (not each item). Hard totals are rounded per constraint.

using DecomposedCalculator = optimaru::DecomposedObjective<SemiconductorSchedule>;
using ConstraintAssembly = optimaru::ConstraintAssembly<SemiconductorSchedule>;
using FabGlobalConstraint = optimaru::GlobalConstraint<SemiconductorSchedule>;

namespace detail {

inline void requirePrepared(const SemiconductorSchedule& schedule) {
    if (!schedule.prepared) {
        throw std::logic_error(
                "fab constraints require "
                "ScoreCalculator::prepare(schedule) first");
    }
}

}  // namespace detail

// The shared per-calculator view: one ScheduleEvaluator, reset once per
// calculator reset, told about every decision-value change, asked for the
// raw totals at most once per score (it caches them until the next change).
struct ScheduleView final : optimaru::SharedConstraintContext {
    void resetOnce(const SemiconductorSchedule& schedule) {
        if (resetGeneration_ == generation) return;
        resetGeneration_ = generation;
        evaluator_.reset(schedule);
    }
    void markChanged(int row) { evaluator_.markChanged(row); }
    void markAllChanged() { evaluator_.markAllChanged(); }
    std::int64_t total(Term term) { return evaluator_.totals()[termIndex(term)]; }

private:
    ScheduleEvaluator evaluator_;
    std::uint64_t resetGeneration_ = ~std::uint64_t{0};
};

// Shared base of the 20 components: the subclass names its Term; the view
// does the rest.
struct ScheduleViewConstraint : FabGlobalConstraint {
    ScheduleViewConstraint(std::string constraint_name, double constraintWeight,
                           optimaru::Level constraintLevel, Term term)
        : FabGlobalConstraint(std::move(constraint_name), constraintWeight, constraintLevel),
          term_(term) {}

    Term term() const { return term_; }

    const char* shared_context_key() const override { return "fab.schedule"; }
    std::unique_ptr<optimaru::SharedConstraintContext> create_shared_context() const override {
        return std::make_unique<ScheduleView>();
    }
    void bind_shared_context(optimaru::SharedConstraintContext& context) override {
        view_ = static_cast<ScheduleView*>(&context);
    }

    void reset(const SemiconductorSchedule& schedule,
               const std::vector<std::vector<int>>& /*members_per_unit*/) override {
        detail::requirePrepared(schedule);
        view_->resetOnce(schedule);
    }
    // A row's value moved: every component hears it; the view keeps one mark.
    bool supports_member_change() const override { return true; }
    void on_member_changed(const SemiconductorSchedule& /*schedule*/, int /*unit*/,
                           const std::vector<int>& /*members*/, int row, bool /*joined*/) override {
        view_->markChanged(row);
    }
    // Not called while supports_member_change() holds; a unit notification
    // cannot say who left, so it re-reads every row.
    void on_unit_changed(const SemiconductorSchedule& /*schedule*/, int /*unit*/,
                         const std::vector<int>& /*members*/) override {
        view_->markAllChanged();
    }
    double cost() const override { return static_cast<double>(view_->total(term_)); }

private:
    Term term_;
    ScheduleView* view_ = nullptr;
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_COMMON_HPP

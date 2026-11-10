#ifndef CS_CONSTRAINTS_COMMON_HPP
#define CS_CONSTRAINTS_COMMON_HPP

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "cs/domain.hpp"
#include "cs/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace cs
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The 36 constraints of the reference conference-scheduling
// ConferenceSchedulingConstraintProvider (16 hard + 20 soft, plus the port's
// synthetic talkTypeIncompatibilityCost), split into one small component each
// on the engine's DecomposedObjective — an optional assembly path NEXT TO
// the monolithic ConferenceSchedulingIncrementalScoreCalculator, which stays
// the tuned default fast path.
//
// Decomposition strategy: BY PROJECTION, not by re-derivation.
// -------------------------------------------------------------------------
// Most of these 36 rules relate talks PAIRWISE — same room, same speaker,
// overlapping timeslots, shared tags, prerequisite chains, same-day grouping,
// per-speaker makespans — so almost none of them decomposes per planning
// value, and the pair semantics (which of `overlap`, `combined duration`,
// `prevailing tags` a rule weighs, and over which join) are intricate enough
// that re-deriving each one independently would be 36 chances to drift from
// the reference.
//
// So every component here is a thin PROJECTION of one term out of
// ScoreCalculator::Breakdown — the very structure the full recompute and the
// monolithic incremental calculator both produce. The components share ONE
// BreakdownProjection (the engine's shared-context contract), which recomputes
// the breakdown at most once per score and hands every component the same
// snapshot; a component then selects its own field and applies its own weight
// and level.
//
// What that buys, which the monolithic path cannot offer:
//   - assemble any SUBSET of the 36 rules;
//   - re-weight or re-level any of them per instance;
//   - read exact per-constraint totals through constraint_totals().
// What it costs: a move on this path is one full O(talks^2) recompute instead
// of an incremental update. That is the trade — this is the composability
// path, not the throughput path. Reach for the monolithic calculator when you
// want speed, and for this one when you want to change the cost function.
//
//   DecomposedCalculator calc;
//   defaultConstraints()                        // the whole default set
//       .weight("contentConflictCost", 500)     // ... then only what differs
//       .soft("roomConflictCost")
//       .drop("speakerMakespanCost")
//       .into(calc);
//   solver.solve(schedule, calc);
//
// Or `addDefaultConstraints(calc)` when nothing differs, and
// `ConstraintAssembly{}` when you would rather list components from
// scratch than start from the defaults.
//
// Each component declares its defaults once, as static constexpr
// kDefaultWeight / kDefaultLevel passed to the base constructor — the Java
// provider's own weights and levels. They are assembly-time metadata, not part
// of the cost. Override them per instance with the constructor's weight
// argument or the mutable `level` / `weight` / `exponent`, or by name through
// ConstraintAssembly. The monolithic calculators (Breakdown::hardTotal() /
// softPenaltyTotal() / softRewardTotal() and the incremental calculator) read
// the same kDefaultWeight constants.
//
// Registry names are the struct names with a lower-case first letter
// ("roomConflictCost"); Python sees them through python_name()
// ("room_conflict_cost"). Each Breakdown member carries the same name.
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty, 0 = no violation, NEGATIVE = reward. The three *DiversityCost rules
// are rewards in the Java provider and return negative costs here (they are
// marked `reward = true; level_flippable = false;`, so hard() and any level
// change are refused), which is why an assembly of all 37 components at their
// default weights reproduces ScoreCalculator::score bit-for-bit,
// rewards included.
//
// Every component requires ScoreCalculator::prepare(schedule) to have run
// (the solve() overload and the monolithic path both call it).

using DecomposedCalculator
    = optimaru::DecomposedObjective<ConferenceSchedule>;
using ConstraintAssembly = optimaru::ConstraintAssembly<ConferenceSchedule>;
using CsUnitConstraint = optimaru::UnitConstraint<ConferenceSchedule>;
using CsGlobalConstraint = optimaru::GlobalConstraint<ConferenceSchedule>;

// A component's kDefaultWeight as an exact integer, for the monolithic
// calculators (Breakdown's weighted totals and the incremental calculator),
// which are the default assembly computed in int64 like the Java long
// arithmetic. Fails to compile if a default weight stops being integral or if
// a component leaves the level the monolith sums it into.
template <typename Component, optimaru::Level kLevel>
constexpr std::int64_t monolithicWeight()
{
    static_assert(Component::kDefaultLevel == kLevel,
                  "the monolithic calculators sum this constraint into "
                  "another level");
    constexpr auto weight
        = static_cast<std::int64_t>(Component::kDefaultWeight);
    static_assert(weight == Component::kDefaultWeight,
                  "the monolithic calculators assume integral default "
                  "weights");
    return weight;
}
template <typename Component>
constexpr std::int64_t kHardWeight
    = monolithicWeight<Component, optimaru::Level::Hard>();
template <typename Component>
constexpr std::int64_t kSoftWeight
    = monolithicWeight<Component, optimaru::Level::Soft>();

// The shared per-score projection: one Breakdown, recomputed lazily and at
// most once however many components read it. Any notification invalidates it;
// the first cost() of the next score rebuilds it.
struct BreakdownProjection final : optimaru::SharedConstraintContext
{
    const Breakdown &of(const ConferenceSchedule &schedule) const
    {
        if (dirty_)
        {
            breakdown_ = ScoreCalculator::breakdownOfPrepared(schedule);
            dirty_ = false;
        }
        return breakdown_;
    }
    void invalidate() { dirty_ = true; }

private:
    mutable Breakdown breakdown_;
    mutable bool dirty_ = true;
};

// ---------------------------------------------------------------------------
// Breakdown-projection constraints
// ---------------------------------------------------------------------------
// Shared base of all 36 components: the subclass writes `select` only — which
// term of the breakdown it owns — and everything else (invalidation, the
// shared projection, the level and the weight) comes from here.
struct BreakdownConstraint : CsGlobalConstraint
{
    using CsGlobalConstraint::CsGlobalConstraint;

    // This constraint's raw penalty units out of one breakdown. Reward rules
    // return a NEGATIVE value (see the sign convention above).
    virtual std::int64_t select(const Breakdown &breakdown) const = 0;

    const char *shared_context_key() const override { return "cs.breakdown"; }
    std::unique_ptr<optimaru::SharedConstraintContext>
    create_shared_context() const override
    {
        return std::make_unique<BreakdownProjection>();
    }
    void bind_shared_context(optimaru::SharedConstraintContext &context) override
    {
        projection_ = static_cast<BreakdownProjection *>(&context);
    }

    void
    reset(const ConferenceSchedule &schedule,
          const std::vector<std::vector<int>> & /*members_per_unit*/) override
    {
        if (!schedule.prepared)
        {
            throw std::logic_error("cs constraints require "
                                   "ScoreCalculator::prepare(schedule) first");
        }
        schedule_ = &schedule;
        projection_->invalidate();
    }
    void on_unit_changed(const ConferenceSchedule &schedule,
                       int /*unit*/,
                       const std::vector<int> & /*members*/) override
    {
        schedule_ = &schedule;
        projection_->invalidate();
    }
    double cost() const override
    {
        return static_cast<double>(select(projection_->of(*schedule_)));
    }

private:
    const ConferenceSchedule *schedule_ = nullptr;
    BreakdownProjection *projection_ = nullptr;
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_COMMON_HPP

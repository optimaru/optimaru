// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SCORE_DECOMPOSED_OBJECTIVE_HPP
#define OPTIMARU_SCORE_DECOMPOSED_OBJECTIVE_HPP

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/score/objective.hpp"
#include "optimaru/score/levels.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Decomposed score calculation
// ---------------------------------------------------------------------------
// Cost contract generalized over both solution shapes:
// a constraint declares that its cost decomposes as a sum over "units", and
// only writes the per-unit evaluation — the objective below turns that into
// a correct Objective by caching per-unit contributions and
// recomputing only the units a move touched.
//
//   sequence shape:  unit = one list (a route); members = its elements in order.
//               A route-level recalculation hook.
//   assignment shape: unit = one value (e.g. an employee); members = the entities
//               currently assigned to it (order NOT meaningful).
//
// Cost sign convention: positive = penalty, negative = reward (raw 0 = no
// violation), total score component = -(weight * cost). `level` routes the
// contribution into the hard accumulator (rounded per unit contribution —
// hard costs should be integral), the medium accumulator or the soft one.
// The objective keeps the three apart and folds medium into the engine's
// Score with the domain's MediumFoldOf<Solution> (see levels.hpp)
// when it reports a score. Contributions and totals follow "The range of a
// score" (levels.hpp): a non-finite contribution, or a hard one of 2^62 or
// more, throws std::overflow_error; the hard total is summed exactly
// (HardTotal) and saturates at +-INT64_MAX instead of wrapping.
//
// By default a touched unit is re-costed from scratch (correct for any
// cost()). A constraint that also implements the optional delta_cost() —
// the marginal cost of one member joining/leaving its unit — is maintained
// incrementally instead, which brings an assembly within a small constant
// factor of a fully hand-written objective while keeping cost definitions
// one small class each — and per-constraint score totals for free.

// Sign-preserving power shaping: raise a cost's magnitude to `exponent`
// without flipping rewards (negative costs) into penalties. exponent == 1 is
// the identity; 2/3 make concentrated per-unit violations disproportionately
// expensive (a superlinear penalty curve).
inline double shape_cost(double cost, double exponent) {
    if (exponent == 1.0 || cost == 0.0) return cost;
    return cost < 0.0 ? -std::pow(-cost, exponent) : std::pow(cost, exponent);
}

// The full shaping of one raw cost x (one unit of a unit constraint, or a
// global constraint's single total) with the exponent, offset and clip knobs.
// This is the canonical statement of the formula; the other comments refer
// here.
//
//   clip = true  (threshold, the default):
//       sign(x) * max(0, |x| - offset) ^ exponent
//       Only the part of |x| beyond the offset counts; x = 0 stays 0 at any
//       offset (sign(0) = 0). A negative offset adds |offset| to every
//       non-zero x: a fixed charge per non-zero unit, so the shaping jumps
//       at x = 0 (see shaping_jumps_at_zero).
//   clip = false (the literal formula):
//       shape_cost(x - offset, exponent)
//       Every evaluated unit counts, a raw 0 included: below the offset the
//       shaped value is negative, i.e. a reward ((x - 10)^3 at offset 10).
//
// The formula applies per evaluated unit (per vehicle, per slot, ...), not
// once over the total violation. offset = 0 is shape_cost(x, exponent) in both
// modes, bit for bit (the plain shaping). The Python mirror (_constraint.py
// shape) is the same formula, step for step, so both give the same double.
inline double shape_cost(double cost, double exponent, double offset, bool clip) {
    if (offset == 0.0) return shape_cost(cost, exponent);
    if (!clip) return shape_cost(cost - offset, exponent);
    if (cost == 0.0) return 0.0;
    const double excess = std::fabs(cost) - offset;
    if (!(excess > 0.0)) return 0.0;
    const double shaped = shape_cost(excess, exponent);
    return cost < 0.0 ? -shaped : shaped;
}

// Is the shaping discontinuous at x = 0? Only the threshold with a negative
// offset is: 0 at x == 0 exactly, |offset|^exponent just beside it. Every
// other combination is continuous there, so a rounding residue on a raw cost
// that should be 0 (raw + d - d with real-valued deltas) shifts its
// contribution by a rounding residue too. Here it would be charged the whole
// fixed charge, so the objective gives such a constraint its raw costs
// exactly, as cost() computes them, instead of accumulating deltas
// (compile_unit_dispatch, and the previews: rebuild).
inline bool shaping_jumps_at_zero(double offset, bool clip) { return clip && offset < 0.0; }

// One contribution: weight * shape_cost(cost, exponent[, offset, clip]), and
// exactly 0 at weight 0 without shaping the cost at all. An opt-in
// constraint left at weight 0 contributes nothing whatever its exponent or
// offset: shaping first would give 0 * inf = NaN once cost^exponent leaves
// the double range, and that NaN would be refused as a non-finite
// contribution (levels.hpp, rule 1). Every contribution the objective
// computes goes through here; the Python score() mirror (_constraint.py
// _contribution) follows the same rule.
inline double weighted_cost(double weight, double cost, double exponent) {
    if (weight == 0.0) return 0.0;
    return weight * shape_cost(cost, exponent);
}
inline double weighted_cost(double weight, double cost, double exponent, double offset, bool clip) {
    if (weight == 0.0) return 0.0;
    return weight * shape_cost(cost, exponent, offset, clip);
}

// Objective-owned, type-erased state shared by related constraints. The
// objective advances the generation once per rebuild(), letting
// a domain rebuild a common projection exactly once without process-global or
// thread-local caches.
struct SharedConstraintContext {
    virtual ~SharedConstraintContext() = default;
    std::uint64_t generation = 0;
    void begin_reset() { ++generation; }

    // sequence shape: called by the objective once per shared context when a list
    // has changed (after the change, before the constraints re-cost it) and
    // once per list at reset, after every constraint's on_reset()/reset(). The
    // place for a per-list projection several components read, such as the
    // prefix sums behind a piecewise preview (see UnitConstraint::
    // piecewise_cost): one notification per change however many components
    // share it. The context reads the lists from the solution a constraint
    // handed it in on_reset(). A reset follows every rejected compound move
    // (Snapshot::restore), so a context that rebuilds eagerly here
    // pays a full rebuild per reset; marking the list and rebuilding on the
    // first read is the cheaper design (see the VRP RouteSumsContext).
    virtual void on_list_changed(int /*list*/) {}
};

template <typename Solution>
struct UnitConstraint;

// The sign rule's exception. A constraint whose raw cost can be negative on
// the problem's data although it is not a reward (a signed objective, such as
// quadratic assignment's unvalidated cost matrices: raw_cost_can_be_negative) can
// no more count toward the hard score than a reward can, because a negative
// hard contribution cancels another violation and an infeasible solution
// could score hard 0. Whether it can be negative depends on the data, so the
// objective checks it at reset (DecomposedObjective::validate_level,
// on the level in effect, hard or a below_hard medium). constraint() is the
// registry name; what() names it. The Python bindings raise ValueError with
// message(python_name(constraint()), level()), word for word the text of the
// Python score() mirror (_constraint.py _check_signed), on every path that
// resets an objective (python/constraint_binding.hpp).
class SignedCostLevelError : public std::invalid_argument {
public:
    SignedCostLevelError(std::string constraint, Level level)
        : std::invalid_argument(text("Constraint '" + constraint + "'", level)),
          constraint_(std::move(constraint)),
          level_(level) {}

    const std::string& constraint() const noexcept { return constraint_; }
    Level level() const noexcept { return level_; }

    // The Python text, the constraint named the way the caller spells it.
    static std::string message(const std::string& constraint, Level level) {
        return text("constraint '" + constraint + "'", level);
    }

private:
    static std::string text(const std::string& who, Level level) {
        const std::string head = who + " can have a negative raw cost on this problem (a bonus) ";
        if (level == Level::Hard) return head + "and cannot be hard";
        return head + "and cannot be medium here, where medium counts toward the hard score";
    }

    std::string constraint_;
    Level level_;
};

// Assembly-time metadata of one constraint, shared by the unit and global
// bases. None of it is part of the cost: a component declares its defaults
// (static constexpr kDefaultWeight / kDefaultLevel, passed to the base
// constructor) and an assembler may re-state them (ConstraintAssembly
// weight/level/hard/soft/exponent) before the objective's next reset.
struct ConstraintMetadata {
    std::string name;
    double weight;  // in units of `level`
    Level level;
    // A reward's raw cost is <= 0 by design (a quantity to maximize, scored as
    // a negative penalty). It can never be hard: ConstraintAssembly::hard()
    // throws and the objective rejects a hard reward at reset.
    bool reward = false;
    // false = the default level is structural: hard()/soft() refuse to move
    // the constraint off it (re-stating the level it already has is fine).
    bool level_flippable = true;
    // Applied BEFORE the weight: contribution = weight * shape_cost(cost,
    // exponent, offset, clip), per unit for a UnitConstraint, on the total
    // for a global (see shape_cost above for the formula).
    double exponent = 1.0;
    // The shift of the shaping (any finite sign; 0 = none) and its mode
    // (clip = true the threshold, false the literal formula): see shape_cost
    // for the formula and python::check_literal_offset for the hard-0 rule,
    // which validate_level enforces at reset.
    double offset = 0.0;
    bool clip = true;
    // Display text for ONE instance of a parametrized constraint that a
    // domain may add several times under the same registry name
    // (employee-scheduling "dayPatternCost" for pattern "A,OFF,B"). Empty =
    // none: the registry name is the label. Never used for lookups: `name`
    // stays the registry name (the catalog, the Python name, the
    // ConstraintAssembly key). constraint_totals()/constraint_costs() report it.
    std::string label;
    // The weight and level the component was constructed with (its
    // kDefaultWeight / kDefaultLevel in every domain factory). The setters
    // below never change them, so constraint_costs() can report the default a
    // pick's weight=None / level=None means even after an assembler re-stated
    // the knobs (Python: the costs entries' default_weight / default_level).
    double default_weight;
    Level default_level;
    // true = this instance is a REPEAT of the first instance of its name in
    // the objective: the same component built with the same domain
    // parameters, so cost(), delta_cost() and piecewise_cost() return what
    // the first instance's return, and only the knobs above differ. The
    // objective then asks one of them and shapes the answer per instance
    // (DecomposedObjective, "Repeats share the raw cost"). Set by
    // ConstraintAssembly::add_repeats(), which the Python bindings use for
    // the second and later picks of a name; never set it on a component
    // whose parameters differ from the first instance's. Read for unit
    // constraints only: every global keeps its own aggregates.
    bool repeats_first = false;

    bool is_hard() const { return level == Level::Hard; }

    ConstraintMetadata(std::string constraint_name, double constraint_weight,
                       Level constraint_level)
        : name(std::move(constraint_name)),
          weight(constraint_weight),
          level(constraint_level),
          default_weight(constraint_weight),
          default_level(constraint_level) {}

    // Copying is how a wrapper component takes over everything its inner
    // component declares (employee-scheduling ScopedUnit):
    //   static_cast<ConstraintMetadata&>(*this) = *inner;
    ConstraintMetadata(const ConstraintMetadata&) = default;
    ConstraintMetadata(ConstraintMetadata&&) noexcept = default;
    ConstraintMetadata& operator=(const ConstraintMetadata&) = default;
    ConstraintMetadata& operator=(ConstraintMetadata&&) noexcept = default;

    // --- the knob rules, in ONE place ------------------------------------
    // Re-state a knob with the checks every client gets: ConstraintAssembly's
    // weight()/level()/hard()/soft()/exponent() call these, and so do the
    // Python bindings (python::apply_knobs) for a component a domain factory
    // builds outside any assembly (employee-scheduling addConstraintByName).
    // Each throws std::invalid_argument naming the constraint:
    //   set_weight    finite and >= 0, in units of the level
    //   set_level     a reward can never be Hard; a constraint whose
    //                level_flippable is false keeps its level (re-stating the
    //                level it already has is a no-op)
    //   set_exponent  finite and > 0
    //   set_offset    finite (any sign)
    //   set_clip      any bool
    void set_weight(double new_weight) {
        if (!std::isfinite(new_weight) || new_weight < 0.0) {
            throw std::invalid_argument("Constraint '" + name + "' weight must be finite and >= 0");
        }
        weight = new_weight;
    }
    void set_level(Level new_level) {
        if (new_level == level) return;
        if (new_level == Level::Hard && reward) {
            throw std::invalid_argument("Constraint '" + name +
                                        "' is a reward (raw cost <= 0) and cannot be hard");
        }
        if (!level_flippable) {
            throw std::invalid_argument("Constraint '" + name + "' is always " + level_name(level) +
                                        "; its level cannot be changed");
        }
        level = new_level;
    }
    void set_exponent(double new_exponent) {
        if (!std::isfinite(new_exponent) || new_exponent <= 0.0) {
            throw std::invalid_argument("Constraint '" + name +
                                        "' exponent must be finite and > 0");
        }
        exponent = new_exponent;
    }
    void set_offset(double new_offset) {
        if (!std::isfinite(new_offset)) {
            throw std::invalid_argument("Constraint '" + name + "' offset must be finite");
        }
        offset = new_offset;
    }
    void set_clip(bool new_clip) { clip = new_clip; }

protected:
    ~ConstraintMetadata() = default;
};

// sequence shape: one run of a list as it is now, from which a previewed list is
// pieced together. The move kinds the engine previews rearrange runs of the
// existing lists, so a candidate list is at most five such pieces (an
// intra-list swap), plus one detached element for preview_insert.
struct ListPiece {
    int list;       // the list the run is taken from; -1 = one detached element
    int begin;      // [begin, end) positions of that list; for a detached
    int end;        // element, begin is the element and end == begin + 1
    bool reversed;  // traversed from end - 1 down to begin

    int size() const { return end - begin; }
    bool detached() const { return list < 0; }
};

// Optional compiled delta group. Constraints with the same structural key are
// collected automatically at reset; groups of two or more replace their
// individual delta_cost() calls with one evaluate() call. The group must write
// one raw marginal cost to `deltas[index]` for every index it owns.
// has_integral_raw_deltas() = every delta it writes is a whole number (counts,
// minutes): the objective then folds hard members at an integral weight
// directly, and keeps members whose shaping jumps at 0 (shaping_jumps_at_zero)
// on the group path, since whole deltas below 2^53 accumulate exactly.
template <typename Solution>
struct UnitConstraintDeltaGroup {
    virtual ~UnitConstraintDeltaGroup() = default;
    virtual void add(std::size_t index, const UnitConstraint<Solution>& constraint) = 0;
    virtual const std::vector<std::size_t>& indexes() const = 0;
    virtual bool is_beneficial() const { return true; }
    virtual bool has_integral_raw_deltas() const { return false; }
    virtual void on_reset(const Solution& solution) = 0;
    virtual void evaluate(const Solution& solution, int unit, const std::vector<int>& members,
                          int entity, double* deltas) = 0;
};

// A cost that is a sum of independent per-unit terms. The exponent, offset
// and clip shape each unit's raw cost before the weight (shape_cost), every
// unit the constraint evaluates (all of them, or its scoped_units()).
//
//   struct FacilityCapacityCost final : optimaru::UnitConstraint<Problem> {
//       static constexpr double kDefaultWeight = 1.0;
//       static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;
//       explicit FacilityCapacityCost(double weight = kDefaultWeight)
//           : UnitConstraint("facilityCapacityCost", weight, kDefaultLevel) {}
//       ...
//   };
template <typename Solution>
struct UnitConstraint : ConstraintMetadata {
    UnitConstraint(std::string constraint_name, double constraint_weight, Level constraint_level)
        : ConstraintMetadata(std::move(constraint_name), constraint_weight, constraint_level) {}
    virtual ~UnitConstraint() = default;

    // Called once per rebuild, before any cost()/delta_cost().
    // Override to precompute lookup tables from the (prepared) solution —
    // e.g. per-(unit, entity) cost terms — so the per-move evaluations become
    // plain array reads. Default: nothing.
    virtual void on_reset(const Solution& /*solution*/) {}

    // The unit's current cost, recomputed from scratch whenever the unit is
    // touched. Must not read state outside (solution, unit, members): in
    // particular no derived values of the solution, since a move preview
    // (sequence shape) evaluates candidate members BEFORE the move, when the
    // derived values still describe the old lists.
    virtual double cost(const Solution& solution, int unit,
                        const std::vector<int>& members) const = 0;

    // Optional incremental contract (assignment shape): when supports_delta() is
    // true, a member joining or leaving a unit is folded into the cached raw
    // cost as `raw ± delta_cost(...)` instead of re-running cost() over the
    // whole unit — O(members) or O(1) per move instead of O(members^2) for
    // pairwise costs. Shaping (exponent) and weight are applied on top of the
    // raw aggregate, so shaped constraints keep the fast path too. One
    // exception: an instance whose shaping jumps at 0 (the threshold with a
    // negative offset, shaping_jumps_at_zero) is re-costed with cost() instead,
    // since a rounding residue of the accumulated raw would be charged in
    // full; only a compiled group with integral raw deltas, which accumulate
    // exactly, keeps it on the delta path.
    //
    // delta_cost returns the marginal raw cost of `entity` belonging to the
    // unit given the OTHER members: implementations iterating `members` must
    // skip `entity` itself, which may or may not be present (it already is
    // right after a join, no longer after a leave). The contract is exactness:
    //   cost(members ∪ {entity}) - cost(members \ {entity}) == delta_cost(...)
    // which holds whenever cost() is a sum of per-member and per-pair terms.
    virtual bool supports_delta() const { return false; }
    virtual double delta_cost(const Solution& /*solution*/, int /*unit*/,
                              const std::vector<int>& /*members*/, int /*entity*/) const {
        return 0.0;
    }

    // Optional sequence shape piecewise preview contract. The objective serves the
    // engine's move previews (see Objective) by re-costing
    // the touched lists on their candidate contents; by default it writes
    // the candidate list out and asks cost(), O(list) per constraint. A
    // constraint whose cost is a function of per-element quantities summed
    // along the list (load, service time, driving time between neighbours)
    // answers instead from prefix sums it keeps per list, in O(pieces):
    // piecewise_cost() receives the candidate list as runs of the current
    // lists (see ListPiece) and returns what cost() would return for the
    // written-out list. Return false to decline one call (a bound the sums
    // cannot settle): the objective then materializes the list and asks
    // cost(). The sums live in a SharedConstraintContext refreshed through
    // its on_list_changed() hook, so several components share one refresh.
    // ScoreChecks::Full checks every preview against the applied score. An instance
    // whose shaping jumps at 0 (shaping_jumps_at_zero) is never asked: sums can
    // round where cost() does not, so its previews always run cost() on the
    // written-out list, which is exactly what the applied path computes.
    virtual bool supports_piecewise_preview() const { return false; }
    virtual bool piecewise_cost(const Solution& /*solution*/, int /*unit*/,
                                const ListPiece* /*pieces*/, int /*count*/, double& /*out*/) const {
        return false;
    }

    // Optional unit-scope contract. Default nullptr = "may be non-zero for ANY
    // unit", so the objective re-costs it on every touched unit. Return the
    // explicit list of unit indices it can contribute to when the constraint is
    // scoped to a subset (e.g. a per-worker rule): the objective then visits it
    // only for those units and skips it everywhere else. The skip is exact ONLY
    // if cost()/delta_cost() are guaranteed 0 for every unit not in the list.
    //
    // The list is also the set of units the constraint EVALUATES (nullptr =
    // every unit): the literal offset formula (clip = false) charges
    // shape_cost(0 - offset) to an evaluated unit whose raw cost is 0 and
    // nothing to a unit outside the scope. constraint_costs() reports the set
    // (`units`), so the Python mirror and the CP-SAT references charge the
    // same units.
    virtual const std::vector<int>* scoped_units() const { return nullptr; }

    // Optional sign contract. A cost is >= 0 by the sign convention, a
    // reward's <= 0. Return true when, on `solution`'s data, this
    // constraint's raw cost can also be negative although it is not a reward
    // (a signed objective over data that is not validated >= 0): the
    // objective then refuses it, at reset, on a level that counts toward the
    // hard score (SignedCostLevelError), as it refuses a hard reward, and
    // constraint_costs() reports it (`signed_cost`). Asked at reset only while
    // the instance sits on such a level, and by constraint_costs(); it reads
    // the problem's facts, not the assignment.
    virtual bool raw_cost_can_be_negative(const Solution& /*solution*/) const { return false; }

    // Optional structural grouping contract. Constraints returning the same
    // non-empty key are compiled into one delta evaluator when at least two
    // compatible constraints are present. Metadata (weight, level, exponent,
    // offset, clip) remains objective-owned and therefore fully customizable.
    virtual const char* delta_group_key() const { return nullptr; }
    virtual std::unique_ptr<UnitConstraintDeltaGroup<Solution>> create_delta_group() const {
        return nullptr;
    }

    // Optional shared-context contract. Constraints returning the same key
    // receive one context owned by this objective instance.
    virtual const char* shared_context_key() const { return nullptr; }
    virtual std::unique_ptr<SharedConstraintContext> create_shared_context() const {
        return nullptr;
    }
    virtual void bind_shared_context(SharedConstraintContext& /*context*/) {}
};

// One list of a previewed move: the unit, its candidate contents as pieces of
// the current lists, and (for constraints without the piecewise contract) the
// same contents materialized. `members` is null while nothing needs it: the
// objective materializes only when a constraint in the assembly lacks the
// piecewise contract or declines a piecewise answer.
struct PreviewedUnit {
    int unit;
    const std::vector<int>* members;
    const ListPiece* pieces;
    int piece_count;
};

// A cost that does NOT decompose per unit (a statistic across units, e.g.
// load balancing). It maintains its own aggregates: notified once per touched
// unit, asked for the closed-form total at scoring time. The exponent, offset
// and clip shape that single total.
//
// The objective never accumulates a global's total: it asks cost() (or
// preview_cost()) at every score, so the total is exactly what the global's
// own aggregates give. That matters for a threshold with a negative offset,
// whose shaping jumps at 0 (shaping_jumps_at_zero): a global whose total can be
// fractional should not keep it as a running sum of real-valued differences,
// or a total that should be 0 may carry a rounding residue that is charged
// the whole |offset|^exponent. Integral aggregates (counts, int64 sums) and
// totals recomputed from the members are exact; every global in this
// repository's use cases is one of those.
template <typename Solution>
struct GlobalConstraint : ConstraintMetadata {
    GlobalConstraint(std::string constraint_name, double constraint_weight, Level constraint_level)
        : ConstraintMetadata(std::move(constraint_name), constraint_weight, constraint_level) {}
    virtual ~GlobalConstraint() = default;

    virtual void reset(const Solution& solution,
                       const std::vector<std::vector<int>>& members_per_unit) = 0;
    virtual void on_unit_changed(const Solution& solution, int unit,
                                 const std::vector<int>& members) = 0;
    // Optional assignment-variable fast path. When enabled, the objective sends
    // every join/leave directly (including inside a composite batch), avoiding
    // a full-unit diff/copy for global aggregates that can update in O(1).
    virtual bool supports_member_change() const { return false; }
    virtual void on_member_changed(const Solution& solution, int unit,
                                   const std::vector<int>& members, int /*entity*/,
                                   bool /*joined*/) {
        on_unit_changed(solution, unit, members);
    }
    virtual double cost() const = 0;

    // Optional weight-0 contract. A global left at weight 0 (an opt-in
    // objective that is off) adds nothing to the score, but by default the
    // objective keeps sending it every change: a global may serve more than
    // its own cost (aggregates that a move source or another component
    // reads). Return true when nothing but this global's own cost() reads
    // what it keeps and reset() rebuilds all of it from (solution, members):
    // the objective then sends it no change while its weight is 0, does not
    // ask its cost() for a score, and resets it on the current members before
    // constraint_costs() reports its raw cost. That reset can come at any
    // point between two changes, inside an open batch or trial too, and must
    // leave nothing behind that a later change relies on. A unit constraint
    // needs no such answer: its cost() is a function of (solution, unit,
    // members), so the objective leaves every unit constraint at weight 0 out
    // of its moves (compile_unit_dispatch).
    virtual bool idle_at_weight_zero() const { return false; }

    // The sign contract of UnitConstraint::raw_cost_can_be_negative, for the
    // single total.
    virtual bool raw_cost_can_be_negative(const Solution& /*solution*/) const { return false; }

    // Optional sequence shape preview contract. The objective serves the engine's
    // move previews (see Objective) by re-costing the touched
    // lists on their candidate contents; a global constraint takes part by
    // answering what cost() WOULD be if each `changes[i].unit` held
    // `*changes[i].members` instead of its current members (at most two units
    // for the built-in moves), without mutating itself. The objective serves
    // previews only when every global in the assembly supports them; one that
    // does not switches the whole assembly back to apply/score/undo.
    virtual bool supports_preview() const { return false; }
    virtual double preview_cost(const Solution& /*solution*/, const PreviewedUnit* /*changes*/,
                                int /*count*/) const {
        return cost();
    }
    // The piecewise form of the same answer (see UnitConstraint::
    // piecewise_cost): read `changes[i].pieces` instead of `*changes[i].members`,
    // which is null until some constraint needs it. Return false to fall back
    // to preview_cost() on the materialized members for this one call.
    virtual bool supports_piecewise_preview() const { return false; }
    virtual bool piecewise_preview_cost(const Solution& /*solution*/,
                                        const PreviewedUnit* /*changes*/, int /*count*/,
                                        double& /*out*/) const {
        return false;
    }

    // Optional shared-context contract, the same one UnitConstraint has:
    // constraints returning the same key receive one context owned by this
    // objective instance. The typical use here is a projection several
    // globals read but none owns — e.g. a per-score recompute that all of
    // them select their own term out of.
    virtual const char* shared_context_key() const { return nullptr; }
    virtual std::unique_ptr<SharedConstraintContext> create_shared_context() const {
        return nullptr;
    }
    virtual void bind_shared_context(SharedConstraintContext& /*context*/) {}
};

// A GlobalConstraint of an assignment-shaped solution that keeps its
// aggregates from joins and leaves alone: the objective hands every one of
// them to on_member_changed (inside a batch too, and the inverse writes of a
// rolled-back move), so an exactly reversible aggregate stays consistent and
// no unit is ever diffed for it. on_unit_changed is therefore unreachable.
template <typename Solution>
struct MemberChangeGlobalConstraint : GlobalConstraint<Solution> {
    using GlobalConstraint<Solution>::GlobalConstraint;

    bool supports_member_change() const final { return true; }
    void on_unit_changed(const Solution& /*solution*/, int /*unit*/,
                         const std::vector<int>& /*members*/) final {
        // A sequence-shaped solution reports list edits unit by unit, to
        // every global: this base cannot serve one.
        static_assert(SolutionTraits<Solution>::shape == Shape::Assignment,
                      "MemberChangeGlobalConstraint is for assignment-shaped solutions");
        throw std::logic_error(this->name +
                               ": on_unit_changed is unreachable (member-change fast path)");
    }
    void on_member_changed(const Solution& solution, int unit, const std::vector<int>& members,
                           int entity, bool joined) override = 0;
};

// One entry of the per-constraint score breakdown (the seed of a score
// analysis / constraint-match view). `cost` is in units of `level`: a medium
// total is NOT multiplied by the domain's medium fold.
struct ConstraintTotal {
    std::string name;
    double cost;   // weighted, sign as defined above (positive = penalizing)
    bool is_hard;  // level == Level::Hard
    Level level = Level::Soft;
    std::string label;  // ConstraintMetadata::label (empty = none)
};

template <typename Solution>
class DecomposedObjective final : public Objective<Solution> {
    using Traits = SolutionTraits<Solution>;

public:
    // How this domain folds its medium accumulator into Score (see
    // levels.hpp). MediumFold::none() unless the domain specializes
    // MediumFoldOf<Solution>.
    static constexpr MediumFold kMediumFold = MediumFoldOf<Solution>::value;

    DecomposedObjective& add(std::unique_ptr<UnitConstraint<Solution>> constraint) {
        bind_shared_context(*constraint);
        unit_constraints_.push_back(std::move(constraint));
        delta_groups_dirty_ = true;
        return *this;
    }
    DecomposedObjective& add(std::unique_ptr<GlobalConstraint<Solution>> constraint) {
        bind_shared_context(*constraint);
        global_constraints_.push_back(std::move(constraint));
        return *this;
    }
    template <typename C, typename... Args>
    DecomposedObjective& emplace(Args&&... args) {
        return add(std::make_unique<C>(std::forward<Args>(args)...));
    }

    void rebuild(Solution& solution) override {
        validate_levels(solution);
        solution_ = &solution;
        if constexpr (Traits::shape == Shape::Assignment) {
            unit_count_ = Traits::value_count(solution);
            members_.assign(unit_count_, {});
            const int entity_count = Traits::entity_count(solution);
            entity_pos_.assign(entity_count, -1);
            for (int e = 0; e < entity_count; ++e) {
                int u = Traits::value_of(solution, e);
                if (u >= 0) {
                    entity_pos_[e] = static_cast<int>(members_[u].size());
                    members_[u].push_back(e);
                }
            }
        } else {
            unit_count_ = list_count_of(solution);
            members_.assign(unit_count_, {});
            for (int l = 0; l < unit_count_; ++l) reload_list_members(l);
        }
        const std::size_t cells = static_cast<std::size_t>(unit_count_) * unit_constraints_.size();
        cache_raw_.assign(cells, 0.0);
        cache_hard_.assign(cells, 0);
        cache_soft_.assign(cells, 0.0);
        delta_capable_.resize(unit_constraints_.size());
        for (auto& [key, context] : shared_contexts_) {
            (void)key;
            context->begin_reset();
        }
        // Preview capabilities, sampled once like the dispatch metadata. A
        // unit constraint at weight 0 (an opt-in objective left off) is not
        // asked on a move at all, previewed or applied: its contribution is 0
        // whatever it answers, and some such costs are the most expensive in
        // an assembly (compile_unit_dispatch puts its entries behind the ones
        // a move visits). This reset costs it, and constraint_costs() costs it
        // again once a move has passed, so its raw cost is still reported. An
        // instance whose shaping jumps at 0 previews through cost() on the
        // written-out candidate, never its piecewise answer (see
        // UnitConstraint::piecewise_cost), so a preview's raw is the applied
        // one exactly.
        piecewise_.resize(unit_constraints_.size());
        for (std::size_t c = 0; c < unit_constraints_.size(); ++c) {
            const UnitConstraint<Solution>& constraint = *unit_constraints_[c];
            delta_capable_[c] = constraint.supports_delta();
            unit_constraints_[c]->on_reset(solution);
            piecewise_[c] = constraint.supports_piecewise_preview() &&
                            !shaping_jumps_at_zero(constraint.offset, constraint.clip);
        }
        reset_delta_groups(solution);
        compile_unit_dispatch();
        // Global dispatch partition: supports_member_change() is a capability
        // query (stable within one solve, like supports_delta()/scoped_units()),
        // so it is sampled once here instead of twice per member-change event.
        // Score-time metadata (weight/exponent/offset/clip/level) is sampled here too —
        // metadata overrides take effect at reset, when the cached per-unit
        // contributions are rebuilt against them anyway.
        // A global that is idle at weight 0 (GlobalConstraint::
        // idle_at_weight_zero) is in none of the three notification lists.
        member_change_globals_.clear();
        unit_change_globals_.clear();
        notified_globals_.clear();
        global_score_.clear();
        for (std::size_t g = 0; g < global_constraints_.size(); ++g) {
            GlobalConstraint<Solution>* pointer = global_constraints_[g].get();
            const bool idle = pointer->weight == 0.0 && pointer->idle_at_weight_zero();
            if (!idle) {
                notified_globals_.push_back(pointer);
                if (pointer->supports_member_change()) {
                    member_change_globals_.push_back(pointer);
                } else {
                    unit_change_globals_.push_back(pointer);
                }
            }
            global_score_.push_back({pointer, pointer->weight, pointer->exponent,
                                     pointer->level == Level::Hard, pointer->level == Level::Medium,
                                     pointer->supports_piecewise_preview(), pointer->offset,
                                     pointer->clip, idle});
        }
        delta_group_ptrs_.clear();
        for (const auto& group : delta_groups_) delta_group_ptrs_.push_back(group.get());
        hard_units_ = HardTotal();
        medium_units_ = 0.0;
        soft_units_ = 0.0;
        unit_hard_.assign(unit_count_, HardTotal());
        penalized_.clear();
        penalized_pos_.assign(unit_count_, -1);
        batch_depth_ = 0;
        dirty_units_.assign(unit_count_, 0);
        dirty_unit_list_.clear();
        trial_active_ = false;
        rollback_active_ = false;
        saved_units_.assign(unit_count_, 0);
        saved_unit_list_.clear();
        saved_raw_.resize(cells);
        saved_hard_.resize(cells);
        saved_soft_.resize(cells);
        saved_unit_hard_.resize(unit_count_);
        for (int u = 0; u < unit_count_; ++u) refresh_unit(u, dispatch_end(u));
        verify_share_classes();
        for (const auto& g : global_constraints_) g->reset(solution, members_);
        moved_since_reset_ = false;
        // Every constraint, unit and global, has seen the solution: the shared
        // contexts may now build whatever they keep per list.
        if constexpr (Traits::shape == Shape::Sequence) {
            for (SharedConstraintContext* context : shared_context_list_) {
                for (int l = 0; l < unit_count_; ++l) context->on_list_changed(l);
            }
        }
    }

    // assignment: the entity leaves its old value's unit and joins the new one.
    // Delta-capable constraints fold the change into the cached raw cost;
    // the rest re-cost the touched unit from scratch.
    //
    // The bodies are behind `if constexpr` because these are virtuals: the
    // vtable instantiates them even for a sequence solution, whose traits have
    // no value_of at all. The engine never calls this pair for a sequence shape.
    void before_assign(int entity_index) override {
        if constexpr (Traits::shape == Shape::Assignment) {
            pending_old_unit_ = Traits::value_of(*solution_, entity_index);
        } else {
            (void)entity_index;
        }
    }
    void after_assign(int entity_index) override {
        if constexpr (Traits::shape != Shape::Assignment) {
            (void)entity_index;
            return;
        } else {
            const int new_unit = Traits::value_of(*solution_, entity_index);
            if (new_unit == pending_old_unit_) return;
            if (pending_old_unit_ >= 0) {
                // Swap-remove via the tracked position (member order carries no
                // meaning for assignment units).
                std::vector<int>& m = members_[pending_old_unit_];
                const int pos = entity_pos_[entity_index];
                const int last = m.back();
                m[pos] = last;
                entity_pos_[last] = pos;
                m.pop_back();
                entity_pos_[entity_index] = -1;
                touch_unit_member_changed(pending_old_unit_, entity_index, /*joined=*/false);
            }
            if (new_unit >= 0) {
                entity_pos_[entity_index] = static_cast<int>(members_[new_unit].size());
                members_[new_unit].push_back(entity_index);
                touch_unit_member_changed(new_unit, entity_index, /*joined=*/true);
            }
        }
    }

    void begin_batch() override { ++batch_depth_; }

    void begin_trial() override {
        // Delta-capable constraints restore themselves through the inverse
        // variable notifications.  Taking a per-unit transaction snapshot in
        // that case only duplicates work on every evaluated move.  Keep the
        // snapshot path for assemblies containing full re-cost constraints,
        // whose expensive rollback recalculation it is designed to avoid
        // (an instance routed there for an exact raw included: the snapshot
        // restores its exact cells).
        if (!has_full_recost_) return;
        trial_active_ = true;
        rollback_active_ = false;
        saved_unit_list_.clear();
        saved_hard_units_total_ = hard_units_;
        saved_medium_units_total_ = medium_units_;
        saved_soft_units_total_ = soft_units_;
    }

    void begin_rollback() override {
        if (trial_active_) rollback_active_ = true;
    }

    void end_trial(bool accepted) override {
        if (!trial_active_) return;
        if (!accepted) {
            for (int unit : saved_unit_list_) {
                const std::size_t base = static_cast<std::size_t>(unit) * unit_constraints_.size();
                const UnitDispatchEntry* const end = dispatch_live_end(unit);
                for (const UnitDispatchEntry* entry = dispatch_begin(unit); entry != end; ++entry) {
                    const std::size_t idx = base + entry->c;
                    cache_raw_[idx] = saved_raw_[idx];
                    cache_hard_[idx] = saved_hard_[idx];
                    cache_soft_[idx] = saved_soft_[idx];
                }
                unit_hard_[unit] = saved_unit_hard_[unit];
            }
            hard_units_ = saved_hard_units_total_;
            medium_units_ = saved_medium_units_total_;
            soft_units_ = saved_soft_units_total_;
            for (int unit : saved_unit_list_) set_penalized(unit, unit_hard_[unit].positive());
        }
        for (int unit : saved_unit_list_) saved_units_[unit] = 0;
        saved_unit_list_.clear();
        trial_active_ = false;
        rollback_active_ = false;
    }

    void end_batch() override {
        if (batch_depth_ <= 0 || --batch_depth_ != 0) return;
        for (int unit : dirty_unit_list_) {
            const std::vector<int>& members = members_[unit];
            if (!rollback_active_) {
                if (has_full_recost_) {
                    const std::size_t base =
                            static_cast<std::size_t>(unit) * unit_constraints_.size();
                    const UnitDispatchEntry* const end = dispatch_live_end(unit);
                    begin_share_pass();
                    for (const UnitDispatchEntry* entry = dispatch_begin(unit); entry != end;
                         ++entry) {
                        if (!is_full_recost(entry->kind)) continue;
                        save_unit_for_trial(unit);
                        apply_raw(unit, *entry, base + entry->c, raw_cost(*entry, unit, members));
                    }
                }
                set_penalized(unit, unit_hard_[unit].positive());
            }
            for (GlobalConstraint<Solution>* global : unit_change_globals_) {
                global->on_unit_changed(*solution_, unit, members);
            }
            dirty_units_[unit] = 0;
        }
        dirty_unit_list_.clear();
    }

    // sequence: the touched list is re-read and re-costed whole (O(list size)).
    void before_list_edit(int /*list_index*/) override {}
    void after_list_edit(int list_index) override {
        moved_since_reset_ = true;
        reload_list_members(list_index);
        for (SharedConstraintContext* context : shared_context_list_) {
            context->on_list_changed(list_index);
        }
        touch_unit(list_index);
    }

    Score score() override {
        const Totals totals = totals_with_globals();
        return fold_score(totals.hard, totals.medium, totals.soft);
    }

    // The three accumulators as score levels (penalties negative), BEFORE the
    // medium fold: what score() folds into its Score. The
    // domain's 3-level Score adapter reads this instead of decoding the fold.
    // The hard level saturates at +-INT64_MAX like score()'s.
    struct LevelScores {
        long long hard;
        double medium;
        double soft;
    };
    LevelScores level_scores() const {
        const Totals totals = totals_with_globals();
        check_soft_total(totals.medium);
        check_soft_total(totals.soft);
        return {-totals.hard.saturated(), totals.medium == 0.0 ? 0.0 : -totals.medium,
                totals.soft == 0.0 ? 0.0 : -totals.soft};
    }

    // Previews answer on a sequence domain whenever every global constraint can
    // preview (see GlobalConstraint::supports_preview) and the working
    // solution is set.
    bool has_previews() const override { return previews_served(); }

    // --- sequence shape: move previews -----------------------------------------
    // Generic, for every assembly: the move's candidate list contents are
    // described as pieces of the current lists (see ListPiece) and every unit
    // constraint is asked what it would cost on them, exactly as
    // after_list_edit() would after the move, so the preview equals the
    // applied score by construction. A constraint with the piecewise contract
    // answers from its prefix sums in O(pieces); any other has the candidate
    // list written into a scratch vector and is asked cost() on it, O(list)
    // — for a cost() that keeps its contract and derives everything from
    // (solution facts, unit, members); one that read a derived values would
    // see the pre-move derived values here, which ScoreChecks::Full reports. Either way a
    // preview is cheaper than the apply/score/undo cycle it replaces (no list
    // mutations, no second re-cost on undo, no derived-state refreshes) and, more
    // importantly, lets the preview-only strategies (iterated local search)
    // run on an assembly at all. Global constraints take part through their
    // preview contract; an assembly with a global that lacks it declines
    // every preview (see GlobalConstraint::supports_preview).
    bool preview_segment_move(int from_list, int start, int len, int to_list, int insert_at,
                              Score& out) override {
        if (!previews_served() || len <= 0) return false;
        const int from_size = static_cast<int>(members_[from_list].size());
        const ListPiece segment{from_list, start, start + len, false};
        PreviewRun runs[2];
        runs[0].unit = from_list;
        runs[0].add({from_list, 0, start, false});
        runs[0].add({from_list, start + len, from_size, false});
        if (from_list == to_list) {
            runs[0].insert_at(insert_at, segment);
            out = preview_score(runs, 1);
            return true;
        }
        const int to_size = static_cast<int>(members_[to_list].size());
        runs[1].unit = to_list;
        runs[1].add({to_list, 0, insert_at, false});
        runs[1].add(segment);
        runs[1].add({to_list, insert_at, to_size, false});
        out = preview_score(runs, 2);
        return true;
    }
    bool preview_swap(int l1, int p1, int l2, int p2, Score& out) override {
        if (!previews_served()) return false;
        PreviewRun runs[2];
        if (l1 == l2) {
            if (p1 == p2) {  // a no-op: the current score, exactly
                out = score();
                return true;
            }
            const int a = std::min(p1, p2), b = std::max(p1, p2);
            const int size = static_cast<int>(members_[l1].size());
            runs[0].unit = l1;
            runs[0].add({l1, 0, a, false});
            runs[0].add({l1, b, b + 1, false});
            runs[0].add({l1, a + 1, b, false});
            runs[0].add({l1, a, a + 1, false});
            runs[0].add({l1, b + 1, size, false});
            out = preview_score(runs, 1);
            return true;
        }
        const int size1 = static_cast<int>(members_[l1].size());
        const int size2 = static_cast<int>(members_[l2].size());
        runs[0].unit = l1;
        runs[0].add({l1, 0, p1, false});
        runs[0].add({l2, p2, p2 + 1, false});
        runs[0].add({l1, p1 + 1, size1, false});
        runs[1].unit = l2;
        runs[1].add({l2, 0, p2, false});
        runs[1].add({l1, p1, p1 + 1, false});
        runs[1].add({l2, p2 + 1, size2, false});
        out = preview_score(runs, 2);
        return true;
    }
    bool preview_reverse(int list, int first, int last, Score& out) override {
        if (!previews_served()) return false;
        const int size = static_cast<int>(members_[list].size());
        PreviewRun run;
        run.unit = list;
        run.add({list, 0, first, false});
        run.add({list, first, last + 1, true});
        run.add({list, last + 1, size, false});
        out = preview_score(&run, 1);
        return true;
    }
    bool preview_tail_swap(int l1, int c1, int l2, int c2, Score& out) override {
        if (!previews_served() || l1 == l2) return false;
        const int size1 = static_cast<int>(members_[l1].size());
        const int size2 = static_cast<int>(members_[l2].size());
        PreviewRun runs[2];
        runs[0].unit = l1;
        runs[0].add({l1, 0, c1, false});
        runs[0].add({l2, c2, size2, false});
        runs[1].unit = l2;
        runs[1].add({l2, 0, c2, false});
        runs[1].add({l1, c1, size1, false});
        out = preview_score(runs, 2);
        return true;
    }
    bool preview_exchange(int l1, int p1, int len1, int l2, int p2, int len2, Score& out) override {
        if (!previews_served() || l1 == l2) return false;
        const int size1 = static_cast<int>(members_[l1].size());
        const int size2 = static_cast<int>(members_[l2].size());
        PreviewRun runs[2];
        runs[0].unit = l1;
        runs[0].add({l1, 0, p1, false});
        runs[0].add({l2, p2, p2 + len2, false});
        runs[0].add({l1, p1 + len1, size1, false});
        runs[1].unit = l2;
        runs[1].add({l2, 0, p2, false});
        runs[1].add({l1, p1, p1 + len1, false});
        runs[1].add({l2, p2 + len2, size2, false});
        out = preview_score(runs, 2);
        return true;
    }
    bool preview_insert(int list, int pos, int element, Score& out) override {
        if (!previews_served()) return false;
        const int size = static_cast<int>(members_[list].size());
        PreviewRun run;
        run.unit = list;
        run.add({list, 0, pos, false});
        run.add({-1, element, element + 1, false});
        run.add({list, pos, size, false});
        out = preview_score(&run, 1);
        return true;
    }

    // Weighted total per constraint, for score explanation ("which constraint
    // costs how much"). Cheap: sums the existing cache. Like score()
    // and constraint_costs(), it uses the knobs sampled at the last reset
    // (SampledKnobs): an edit to a component after the reset takes effect,
    // and is checked (validate_level), at the next one.
    std::vector<ConstraintTotal> constraint_totals() const {
        std::vector<ConstraintTotal> totals;
        const std::size_t C = unit_constraints_.size();
        for (std::size_t c = 0; c < C; ++c) {
            const Level level = unit_knobs(c).level;
            double sum = 0.0;
            for (int u = 0; u < unit_count_; ++u) {
                const std::size_t idx = static_cast<std::size_t>(u) * C + c;
                // Medium cells live in cache_soft_ (one level per constraint).
                sum += level == Level::Hard ? static_cast<double>(cache_hard_[idx])
                                            : cache_soft_[idx];
            }
            totals.push_back({unit_constraints_[c]->name, sum, level == Level::Hard, level,
                              unit_constraints_[c]->label});
        }
        for (std::size_t g = 0; g < global_constraints_.size(); ++g) {
            const GlobalConstraint<Solution>& global = *global_constraints_[g];
            const SampledKnobs knobs = global_knobs(g);
            // At weight 0 the total is 0 whatever cost() says, and an idle
            // global's cost() is not current: not asked.
            totals.push_back({global.name,
                              knobs.weight == 0.0
                                      ? 0.0
                                      : weighted_cost(knobs.weight, global.cost(), knobs.exponent,
                                                      knobs.offset, knobs.clip),
                              knobs.level == Level::Hard, knobs.level, global.label});
        }
        return totals;
    }

    // RAW per-unit costs per constraint — what cost() returned, before the
    // weight, the exponent and the hard rounding. constraint_totals() reports
    // the weighted result; this reports the input to it.
    //
    // It is what lets a caller re-price an assembly WITHOUT re-scoring: the
    // weight/level/exponent arithmetic is a pure function of these numbers
    // (see the Python packages' `score()`), so one scoring pass answers every
    // "what would this cost at weight 10 / as a soft term" question. The raw
    // cost has no weight baked in, so it stays valid as the metadata changes.
    //
    // Per-unit rather than summed because the exponent shapes each unit's cost
    // BEFORE the weight (see apply_raw): a different exponent cannot be
    // recovered from a total. Global constraints own no unit, so they report
    // their single cost() as a one-element list.
    //
    // `units` names the units a SCOPED unit constraint evaluates
    // (scoped_units() != nullptr: the in-range indices, ascending, each once);
    // the other cells of unit_costs are 0 and never evaluated. The literal
    // offset formula (clip = false) charges every EVALUATED unit, a raw 0
    // included, so a re-pricing caller needs that set: `scoped` is false and
    // `units` empty for a constraint that evaluates every unit (and for every
    // global).
    //
    // weight, exponent, level, offset and clip are the knobs the score uses,
    // sampled at the last reset (SampledKnobs), as constraint_totals() reports
    // them. `signed_cost` is raw_cost_can_be_negative() on the working solution's
    // data (false before the first reset): the Python mirror refuses such an
    // entry on a level that counts toward the hard score, as the objective
    // does at reset.
    //
    // A unit constraint at weight 0 and a global that is idle at weight 0 are
    // left out of the moves, so after a move their cells and aggregates are
    // those of the last reset; and a member of a group with integral deltas
    // on the direct hard fold (kGroupedHardInt) keeps its hard cell current
    // but not its raw one. Their raw costs are computed here instead, on the
    // current members: cost() per evaluated unit, and a reset() of the global
    // (its own aggregates only, which is why a const report may do it). So
    // the report describes the current solution for every constraint. Right
    // after a reset nothing is recomputed; after a move every report
    // computes them again. Because of that reset this is not a call for
    // another thread to make while a solve runs on this objective.
    struct ConstraintCosts {
        std::string name;
        double weight;  // the weight in effect (sampled at the last reset)
        double exponent;
        bool is_hard;  // level == Level::Hard
        bool is_global;
        std::vector<double> unit_costs;
        Level level;  // the level in effect (sampled at the last reset)
        bool reward;
        bool level_flippable;
        std::string label;  // ConstraintMetadata::label (empty = none)
        // What weight=None / level=None mean for this component
        // (ConstraintMetadata::default_weight / default_level): equal to
        // weight / level unless the assembly re-stated them.
        double default_weight;
        Level default_level;
        double offset = 0.0;  // the offset / clip in effect (sampled at the last reset)
        bool clip = true;
        bool scoped = false;
        std::vector<int> units;
        bool signed_cost = false;
    };
    std::vector<ConstraintCosts> constraint_costs() const {
        std::vector<ConstraintCosts> out;
        const std::size_t C = unit_constraints_.size();
        out.reserve(C + global_constraints_.size());
        for (std::size_t c = 0; c < C; ++c) {
            const UnitConstraint<Solution>& constraint = *unit_constraints_[c];
            const SampledKnobs knobs = unit_knobs(c);
            std::vector<double> costs;
            // c < unit_knobs_.size(): sampled at the last reset, so the cells
            // below exist (a constraint added since has none yet).
            if (moved_since_reset_ && solution_ != nullptr && c < unit_knobs_.size() &&
                (knobs.weight == 0.0 || (grouped_delta_[c] && direct_grouped_hard_delta_[c]))) {
                costs.assign(static_cast<std::size_t>(unit_count_), 0.0);
                if (const std::vector<int>* scope = constraint.scoped_units()) {
                    for (int u : *scope) {
                        if (u >= 0 && u < unit_count_) {
                            costs[u] = constraint.cost(*solution_, u, members_[u]);
                        }
                    }
                } else {
                    for (int u = 0; u < unit_count_; ++u) {
                        costs[u] = constraint.cost(*solution_, u, members_[u]);
                    }
                }
            } else {
                costs.reserve(static_cast<std::size_t>(unit_count_));
                for (int u = 0; u < unit_count_; ++u) {
                    costs.push_back(cache_raw_[static_cast<std::size_t>(u) * C + c]);
                }
            }
            ConstraintCosts entry{constraint.name,
                                  knobs.weight,
                                  knobs.exponent,
                                  knobs.level == Level::Hard,
                                  false,
                                  std::move(costs),
                                  knobs.level,
                                  constraint.reward,
                                  constraint.level_flippable,
                                  constraint.label,
                                  constraint.default_weight,
                                  constraint.default_level,
                                  knobs.offset,
                                  knobs.clip,
                                  false,
                                  {}};
            entry.signed_cost =
                    solution_ != nullptr && constraint.raw_cost_can_be_negative(*solution_);
            if (const std::vector<int>* scope = constraint.scoped_units()) {
                entry.scoped = true;
                for (int u : *scope) {
                    if (u >= 0 && u < unit_count_) entry.units.push_back(u);
                }
                std::sort(entry.units.begin(), entry.units.end());
                entry.units.erase(std::unique(entry.units.begin(), entry.units.end()),
                                  entry.units.end());
            }
            out.push_back(std::move(entry));
        }
        for (std::size_t g = 0; g < global_constraints_.size(); ++g) {
            const GlobalConstraint<Solution>& global = *global_constraints_[g];
            const SampledKnobs knobs = global_knobs(g);
            if (moved_since_reset_ && solution_ != nullptr && g < global_score_.size() &&
                global_score_[g].constraint == &global && global_score_[g].idle) {
                global_constraints_[g]->reset(*solution_, members_);
            }
            ConstraintCosts entry{global.name,
                                  knobs.weight,
                                  knobs.exponent,
                                  knobs.level == Level::Hard,
                                  true,
                                  {global.cost()},
                                  knobs.level,
                                  global.reward,
                                  global.level_flippable,
                                  global.label,
                                  global.default_weight,
                                  global.default_level,
                                  knobs.offset,
                                  knobs.clip,
                                  false,
                                  {}};
            entry.signed_cost = solution_ != nullptr && global.raw_cost_can_be_negative(*solution_);
            out.push_back(std::move(entry));
        }
        return out;
    }

    const std::vector<std::vector<int>>& members_per_unit() const { return members_; }
    std::size_t delta_group_count() const { return delta_groups_.size(); }

    // Read-only view of the assembly, so a domain can inspect it (e.g. for
    // score explanation or configuration dumps).
    const std::vector<std::unique_ptr<UnitConstraint<Solution>>>& unit_constraint_list() const {
        return unit_constraints_;
    }
    const std::vector<std::unique_ptr<GlobalConstraint<Solution>>>& global_constraint_list() const {
        return global_constraints_;
    }

    // Units whose weighted hard cost is currently > 0, as an unordered list —
    // the generic analog of the hand-written objective's penalizedEmployees(),
    // for violation-guided move selection. O(1) maintenance per touched unit.
    const std::vector<int>& penalized_units() const { return penalized_; }

private:
    // Level sanity, checked at every reset because the metadata is mutable
    // until then (ConstraintAssembly::level / offset / clip, or the setters,
    // in any order): a medium constraint needs a domain medium fold; a
    // reward can never be hard (its raw cost is <= 0, so a hard reward would
    // push the hard score above 0); the hard-0 rule (python::
    // check_literal_offset): no clip false with an offset != 0 on a level that
    // counts toward the hard score; and the sign rule (SignedCostLevelError):
    // no instance whose raw cost can be negative on this problem
    // (raw_cost_can_be_negative) on such a level. The last one reads the data, so
    // it is asked only of an instance that sits on such a level.
    template <typename Constraint>
    static void validate_level(const Constraint& constraint, const Solution& solution) {
        if (constraint.level == Level::Medium && !kMediumFold.has_medium()) {
            throw std::logic_error("Constraint '" + constraint.name +
                                   "' is medium-level, but this domain declares no medium fold "
                                   "(specialize optimaru::MediumFoldOf<Solution>)");
        }
        if (constraint.reward && constraint.level == Level::Hard) {
            throw std::logic_error("Constraint '" + constraint.name +
                                   "' is a reward and cannot be hard");
        }
        if (!constraint.clip && constraint.offset != 0.0 &&
            kMediumFold.counts_toward_hard(constraint.level)) {
            throw std::logic_error("Constraint '" + constraint.name +
                                   "' cannot use clip=false with a non-zero offset on a level that "
                                   "counts toward the hard score (hard 0 must mean no violation)");
        }
        if (!constraint.reward && kMediumFold.counts_toward_hard(constraint.level) &&
            constraint.raw_cost_can_be_negative(solution)) {
            throw SignedCostLevelError(constraint.name, constraint.level);
        }
    }
    void validate_levels(const Solution& solution) const {
        for (const auto& constraint : unit_constraints_) validate_level(*constraint, solution);
        for (const auto& constraint : global_constraints_) validate_level(*constraint, solution);
    }

    // Folds one weighted contribution into the right accumulator (positive =
    // penalty). Hard rounds per contribution; medium rounds too when the fold
    // lands it in the hard level. Both check the range rule of levels.hpp.
    template <typename Entry>
    static void accumulate(const Entry& entry, double value, HardTotal& hard, double& medium,
                           double& soft) {
        const std::string& name = entry.constraint->name;
        if (entry.is_hard) {
            hard.add(round_hard_contribution(value, name));
        } else if (kMediumFold.has_medium() && entry.is_medium) {
            medium += medium_contribution(value, name);
        } else {
            soft += checked_soft_contribution(value, name);
        }
    }

    // One medium contribution: rounded like a hard one when the fold lands
    // medium in the hard level, range-checked like a soft one otherwise.
    static double medium_contribution(double value, const std::string& name) {
        return kMediumFold.rounds_medium()
                       ? static_cast<double>(round_hard_contribution(value, name))
                       : checked_soft_contribution(value, name);
    }

    // The unit totals plus every global's contribution, per level, as
    // penalties (positive = worse) and before the medium fold.
    struct Totals {
        HardTotal hard;
        double medium;
        double soft;
    };
    Totals totals_with_globals() const {
        Totals totals{hard_units_, medium_units_, soft_units_};
        for (const GlobalScoreEntry& entry : global_score_) {
            // An idle global's aggregates are not current: not asked. Any
            // other global at weight 0 is asked as it always was (its
            // contribution is an exact 0): only one that declares itself
            // idle is treated differently from before.
            if (entry.idle) continue;
            accumulate(entry, global_contribution(entry, entry.constraint->cost()), totals.hard,
                       totals.medium, totals.soft);
        }
        return totals;
    }

    // A soft total of finite contributions can still overflow a double.
    static void check_soft_total(double total) {
        if (!std::isfinite(total)) {
            throw std::overflow_error("the soft score is not finite (" + std::to_string(total) +
                                      "); lower the soft weights or exponents");
        }
    }

    // Accumulated penalties -> the engine score, through the domain's medium
    // fold. The hard level saturates at +-INT64_MAX (levels.hpp, rule 2).
    // Normalizes -0.0 to 0.0: a penalty-free assembly should report the
    // same "0soft" a hand-written objective does, not "-0".
    static Score fold_score(const HardTotal& hard, double medium, double soft) {
        std::int64_t penalty;
        if constexpr (kMediumFold.kind == MediumFold::Kind::IntoSoft) {
            soft += medium * kMediumFold.unit;
            penalty = hard.saturated();
        } else if constexpr (kMediumFold.kind == MediumFold::Kind::BelowHard) {
            penalty = fold_below_hard_saturating(hard, medium,
                                                 static_cast<std::int64_t>(kMediumFold.unit));
        } else {
            (void)medium;
            penalty = hard.saturated();
        }
        check_soft_total(soft);
        return {-penalty, soft == 0.0 ? 0.0 : -soft};
    }

    // Hands `constraint` the context its key names, creating it on first use.
    // Shared by both add() overloads, so a unit and a global constraint asking
    // for the same key get the very same instance.
    template <typename Constraint>
    void bind_shared_context(Constraint& constraint) {
        const char* context_key = constraint.shared_context_key();
        if (context_key == nullptr || *context_key == '\0') return;
        auto [it, inserted] = shared_contexts_.try_emplace(context_key);
        if (inserted) it->second = constraint.create_shared_context();
        if (!it->second) {
            throw std::logic_error("Constraint shared context '" + std::string(context_key) +
                                   "' has no factory");
        }
        if (inserted) shared_context_list_.push_back(it->second.get());
        constraint.bind_shared_context(*it->second);
    }

    // How one (unit, constraint) pair is handled on the member-change hot
    // path, decided once per reset (capability queries and metadata are
    // sampled at rebuild, like delta_capable_ always was).
    enum DispatchKind : std::uint8_t {
        kGroupedHardInt = 0,  // grouped delta, hard, integral weight: direct fold
        kGroupedApply = 1,    // grouped delta folded through apply_raw (soft/shaped)
        kVirtualDelta = 2,    // per-constraint delta_cost() call
        kFullRecost = 3,      // full cost() re-run: no delta contract, or a raw that
                              // must be exact (shaping_jumps_at_zero)
        // The same two for an instance of a share class ("Repeats share the
        // raw cost" below): the delta or the cost is asked of one instance
        // per unit event and reused by the others.
        kSharedVirtualDelta = 4,
        kSharedFullRecost = 5,
    };
    static bool is_full_recost(std::uint8_t kind) {
        return kind == kFullRecost || kind == kSharedFullRecost;
    }
    static bool is_shared(std::uint8_t kind) { return kind >= kSharedVirtualDelta; }
    // One compiled dispatch entry: everything the hot path needs, contiguous
    // (the constraint pointer for the virtual call, the classification, and
    // the reset-sampled metadata apply_raw uses).
    struct UnitDispatchEntry {
        UnitConstraint<Solution>* constraint;
        double weight;
        double exponent;
        std::uint32_t c;  // constraint index (cache/buffer addressing)
        std::uint8_t kind;
        bool is_hard;
        bool is_medium;
        // offset != 0: the shaping reads unit_offset_[c] / unit_clip_[c]. A flag
        // in the entry's padding, so an unshifted assembly
        // keeps its 32-byte entries and its plain weighted_cost().
        bool shifted;
    };

    // The compiled entries of one unit: [dispatch_begin, dispatch_end).
    const UnitDispatchEntry* dispatch_begin(int unit) const {
        return dispatch_.data() + dispatch_start_[unit];
    }
    const UnitDispatchEntry* dispatch_end(int unit) const {
        return dispatch_.data() + dispatch_start_[unit + 1];
    }
    // The end of the entries a move visits: [dispatch_begin, dispatch_live_end)
    // are the unit's constraints with a weight, [dispatch_live_end,
    // dispatch_end) the ones at weight 0, which only a reset costs.
    const UnitDispatchEntry* dispatch_live_end(int unit) const {
        return dispatch_.data() + dispatch_live_end_[unit];
    }

    // --- Repeats share the raw cost ----------------------------------------
    // A constraint picked several times is several components that differ
    // only in their knobs (ConstraintMetadata::repeats_first), so they all
    // have the same raw cost and the same delta. compile_unit_dispatch puts
    // such instances into a share class, and within one pass over a unit's
    // dispatch entries (a member change, a re-cost, a batch end, one list of
    // a preview) the first entry of a class that needs the value asks its
    // component and the others reuse the answer: N picks cost one cost() or
    // delta_cost() call instead of N. Every instance still keeps its own
    // cells (raw, hard, soft) and its own shaping, so nothing else changes:
    // the score is bit for bit the one N independent components give.
    //
    // A pass is identified by share_pass_; share_stamp_[k] says whether class
    // k already holds this pass's value in share_value_[k].
    void begin_share_pass() const { ++share_pass_; }
    template <typename Compute>
    double shared_value(const UnitDispatchEntry& entry, Compute&& compute) const {
        const std::uint32_t k = share_class_[entry.c];
        if (share_stamp_[k] == share_pass_) return share_value_[k];
        const double value = compute();
        share_stamp_[k] = share_pass_;
        share_value_[k] = value;
        return value;
    }
    // cost() of one entry's component on a unit, through its share class.
    double raw_cost(const UnitDispatchEntry& entry, int unit,
                    const std::vector<int>& members) const {
        if (!is_shared(entry.kind)) return entry.constraint->cost(*solution_, unit, members);
        return shared_value(entry,
                            [&] { return entry.constraint->cost(*solution_, unit, members); });
    }

    // The share class of every unit constraint: the index of the earliest
    // instance it shares with (itself when it shares with none). Instance c
    // joins the class of the first earlier instance f of its name that is the
    // name's first instance or another repeat, and that the objective handles
    // the same way, so one answer serves both:
    //   - the same dispatch kind, kVirtualDelta or kFullRecost (a repeat
    //     whose shaping jumps at 0 re-costs while a plain one takes deltas,
    //     and the members of a compiled delta group are evaluated together
    //     by the group already);
    //   - the same preview path (piecewise_);
    //   - the same scope (scoped_units()), so both sit in the same units'
    //     dispatch entries.
    // Instances that match no earlier one stay on their own, as before.
    // Declaring a repeat whose first instance is a component of another type
    // is a programming error and throws (other instances of the name that
    // are neither the first nor repeats are simply not shared with). A repeat
    // of the right type built with other domain parameters is caught by
    // verify_share_classes at every reset.
    //
    // Runs after has_full_recost_ was read off the unshared kinds, and turns
    // kFullRecost into kSharedFullRecost: test a kind with is_full_recost().
    void compile_share_classes(std::vector<UnitDispatchEntry>& metas) {
        const std::size_t C = unit_constraints_.size();
        share_class_.resize(C);
        share_stamp_.assign(C, 0);
        share_value_.assign(C, 0.0);
        share_pass_ = 0;
        std::vector<std::size_t> class_size(C, 1);
        auto same_scope = [](const std::vector<int>* a, const std::vector<int>* b) {
            return (a == nullptr) == (b == nullptr) && (a == nullptr || *a == *b);
        };
        for (std::size_t c = 0; c < C; ++c) {
            share_class_[c] = static_cast<std::uint32_t>(c);
            const UnitConstraint<Solution>& constraint = *unit_constraints_[c];
            if (!constraint.repeats_first) continue;
            bool first_seen = false;
            for (std::size_t f = 0; f < c; ++f) {
                const UnitConstraint<Solution>& earlier = *unit_constraints_[f];
                if (earlier.name != constraint.name) continue;
                const bool is_first = !first_seen;
                first_seen = true;
                if (typeid(earlier) != typeid(constraint)) {
                    if (!is_first) continue;
                    throw std::logic_error("Constraint '" + constraint.name +
                                           "' is declared a repeat of the first instance of its "
                                           "name, which is a different component");
                }
                if (!is_first && !earlier.repeats_first) continue;
                if (share_class_[f] != f) continue;  // its class's first instance was tried already
                if (metas[f].kind != metas[c].kind ||
                    (metas[c].kind != kVirtualDelta && metas[c].kind != kFullRecost) ||
                    piecewise_[f] != piecewise_[c] ||
                    !same_scope(earlier.scoped_units(), constraint.scoped_units())) {
                    continue;
                }
                share_class_[c] = static_cast<std::uint32_t>(f);
                ++class_size[f];
                break;
            }
        }
        for (std::size_t c = 0; c < C; ++c) {
            if (class_size[share_class_[c]] < 2) continue;
            metas[c].kind =
                    metas[c].kind == kVirtualDelta ? kSharedVirtualDelta : kSharedFullRecost;
        }
    }

    // The check behind the sharing, at every reset (after the units were
    // re-costed): each instance of a share class is asked its own cost() and
    // must return the raw cost its class computed, bit for bit. A component
    // declared a repeat although its parameters differ from the first
    // instance's fails here on the first solution where they matter, which
    // ScoreChecks::Full (a reset per check) turns into a check of every step.
    // A reset costs every instance once, as it did before sharing.
    void verify_share_classes() const {
        for (int unit = 0; unit < unit_count_; ++unit) {
            const std::size_t base = static_cast<std::size_t>(unit) * unit_constraints_.size();
            const UnitDispatchEntry* const end = dispatch_end(unit);
            for (const UnitDispatchEntry* entry = dispatch_begin(unit); entry != end; ++entry) {
                if (!is_shared(entry->kind) || share_class_[entry->c] == entry->c) continue;
                const double own = entry->constraint->cost(*solution_, unit, members_[unit]);
                if (own != cache_raw_[base + entry->c]) {
                    char values[80];  // every digit: the two may differ in the last bit only
                    std::snprintf(values, sizeof values, "%.17g against %.17g", own,
                                  cache_raw_[base + entry->c]);
                    throw std::logic_error(
                            "Constraint '" + entry->constraint->name +
                            "' is declared a repeat of the first instance of its name "
                            "(add_repeats), but its raw cost differs from that instance's (" +
                            values + " on unit " + std::to_string(unit) +
                            "); add it with add_instances()");
                }
            }
        }
    }

    // One unit's shaped, weighted contribution (the knobs sampled at reset).
    double unit_contribution(const UnitDispatchEntry& entry, double raw) const {
        if (!entry.shifted) return weighted_cost(entry.weight, raw, entry.exponent);
        return weighted_cost(entry.weight, raw, entry.exponent, unit_offset_[entry.c],
                             unit_clip_[entry.c] != 0);
    }

    // Compiles the per-unit dispatch table: one contiguous, pre-classified
    // entry per (unit, applicable constraint), so the per-event hot path is
    // one switch on `kind` instead of per-constraint capability probes and
    // scattered metadata loads. A unit visits every "applies-to-all"
    // constraint (scoped_units()==nullptr) plus the ones explicitly scoped to
    // it. Constraints scoped to a subset of units are thereby skipped on the
    // ~all other units they can only contribute 0 to — the dominant waste
    // when many per-worker rules exist. All cost/save/restore loops iterate
    // this instead of [0,C), so scoped-out (unit,c) cells stay at their reset
    // value (0) forever, which is exactly their true cost.
    //
    // Within a unit the entries of constraints at weight 0 come last, behind
    // dispatch_live_end: a move (a member change, a list edit, a batch end, a
    // preview, the trial snapshot) visits only the entries before it, in the
    // order they always had. A weight-0 entry adds an exact 0 to every total
    // (weighted_cost), so leaving it out changes no score, bit for bit; its
    // raw cell then describes the last reset, and constraint_costs() costs
    // such a constraint itself once a move has passed. A full re-cost at
    // weight 0 does not count for has_full_recost_ either: no trial snapshot
    // is taken for cells no move writes. One thing a move still does for a
    // constraint at weight 0: a member of a compiled delta group is evaluated
    // with its group (the group computes all its members' deltas in one
    // pass), and only its fold is left out.
    //
    // An instance whose shaping jumps at 0 (shaping_jumps_at_zero: the threshold
    // with a negative offset) needs its raw exactly as cost() gives it: a
    // raw accumulated as cache_raw_ + sign * delta (kGroupedApply,
    // kVirtualDelta) can keep a residue of 1e-16 where it should be back at
    // 0, which the shaping charges the whole |offset|^exponent. Such an
    // instance takes kFullRecost instead, on the applies, in a batch
    // (end_batch) and through the trial snapshot (a rejected move
    // restores the saved exact cells), like a constraint without a delta
    // contract. The one exception is a member of a compiled group with
    // integral raw deltas (has_integral_raw_deltas: school timetabling's slot
    // group, employee scheduling's assignment group): integral deltas below
    // 2^53 accumulate exactly, so it keeps kGroupedApply. kGroupedHardInt
    // never meets the case (it needs offset 0). Every other combination
    // keeps its dispatch: clip=false with any offset and the threshold with
    // offset >= 0 are continuous at 0, so a residue there stays a residue.
    void compile_unit_dispatch() {
        const std::size_t C = unit_constraints_.size();
        std::vector<UnitDispatchEntry> metas(C);
        unit_offset_.assign(C, 0.0);
        unit_clip_.assign(C, 1);
        unit_knobs_.clear();
        unit_knobs_.reserve(C);
        has_full_recost_ = false;
        for (std::size_t c = 0; c < C; ++c) {
            const UnitConstraint<Solution>& constraint = *unit_constraints_[c];
            UnitDispatchEntry& meta = metas[c];
            meta.constraint = unit_constraints_[c].get();
            meta.weight = constraint.weight;
            meta.exponent = constraint.exponent;
            meta.c = static_cast<std::uint32_t>(c);
            const bool exact_raw = shaping_jumps_at_zero(constraint.offset, constraint.clip);
            if (!delta_capable_[c]) {
                meta.kind = kFullRecost;
            } else if (grouped_delta_[c]) {
                meta.kind = direct_grouped_hard_delta_[c]              ? kGroupedHardInt
                            : exact_raw && !integral_grouped_delta_[c] ? kFullRecost
                                                                       : kGroupedApply;
            } else {
                meta.kind = exact_raw ? kFullRecost : kVirtualDelta;
            }
            has_full_recost_ |= meta.kind == kFullRecost && meta.weight != 0.0;
            meta.is_hard = constraint.level == Level::Hard;
            meta.is_medium = constraint.level == Level::Medium;
            meta.shifted = constraint.offset != 0.0;
            unit_offset_[c] = constraint.offset;
            unit_clip_[c] = constraint.clip ? 1 : 0;
            unit_knobs_.push_back(live_knobs(constraint));
        }
        compile_share_classes(metas);
        std::vector<std::size_t> apply_all;
        for (std::size_t c = 0; c < C; ++c)
            if (unit_constraints_[c]->scoped_units() == nullptr) apply_all.push_back(c);
        dispatch_start_.assign(static_cast<std::size_t>(unit_count_) + 1, 0);
        for (std::size_t c = 0; c < C; ++c) {
            const std::vector<int>* su = unit_constraints_[c]->scoped_units();
            if (su == nullptr) continue;
            for (int u : *su)
                if (u >= 0 && u < unit_count_) ++dispatch_start_[u + 1];
        }
        std::size_t total = 0;
        for (int u = 0; u < unit_count_; ++u) {
            total += apply_all.size() + dispatch_start_[u + 1];
            dispatch_start_[u + 1] = total;
        }
        dispatch_.resize(total);
        dispatch_live_end_.assign(static_cast<std::size_t>(unit_count_), 0);
        std::vector<std::size_t> cursor(unit_count_);
        for (int u = 0; u < unit_count_; ++u) cursor[u] = dispatch_start_[u];
        // Two passes in the same order: the constraints with a weight, then
        // the ones at weight 0.
        for (const bool live : {true, false}) {
            for (int u = 0; u < unit_count_; ++u) {
                for (std::size_t c : apply_all) {
                    if ((metas[c].weight != 0.0) == live) dispatch_[cursor[u]++] = metas[c];
                }
            }
            for (std::size_t c = 0; c < C; ++c) {
                const std::vector<int>* su = unit_constraints_[c]->scoped_units();
                if (su == nullptr || (metas[c].weight != 0.0) != live) continue;
                for (int u : *su)
                    if (u >= 0 && u < unit_count_) dispatch_[cursor[u]++] = metas[c];
            }
            if (live) {
                for (int u = 0; u < unit_count_; ++u) dispatch_live_end_[u] = cursor[u];
            }
        }
    }

    void reset_delta_groups(const Solution& solution) {
        if (delta_groups_dirty_) {
            rebuild_delta_groups(solution);
            delta_groups_dirty_ = false;
            return;
        }
        refresh_direct_grouped_hard_deltas();
        for (const auto& group : delta_groups_) group->on_reset(solution);
    }

    void refresh_direct_grouped_hard_deltas() {
        std::fill(direct_grouped_hard_delta_.begin(), direct_grouped_hard_delta_.end(), 0);
        std::fill(integral_grouped_delta_.begin(), integral_grouped_delta_.end(), 0);
        for (const auto& group : delta_groups_) {
            if (!group->has_integral_raw_deltas()) continue;
            for (std::size_t index : group->indexes()) {
                const UnitConstraint<Solution>& constraint = *unit_constraints_[index];
                integral_grouped_delta_[index] = 1;
                // The direct fold adds weight * raw delta: exact only for an
                // unshaped contribution (exponent 1, no offset).
                direct_grouped_hard_delta_[index] =
                        constraint.level == Level::Hard && constraint.exponent == 1.0 &&
                        constraint.offset == 0.0 && std::isfinite(constraint.weight) &&
                        std::trunc(constraint.weight) == constraint.weight;
            }
        }
    }

    void rebuild_delta_groups(const Solution& solution) {
        delta_groups_.clear();
        grouped_delta_.assign(unit_constraints_.size(), 0);
        direct_grouped_hard_delta_.assign(unit_constraints_.size(), 0);
        integral_grouped_delta_.assign(unit_constraints_.size(), 0);
        grouped_delta_buffer_.assign(unit_constraints_.size(), 0.0);

        std::vector<std::unique_ptr<UnitConstraintDeltaGroup<Solution>>> candidates;
        std::unordered_map<std::string, std::size_t> group_by_key;
        for (std::size_t c = 0; c < unit_constraints_.size(); ++c) {
            if (!delta_capable_[c]) continue;
            const char* key = unit_constraints_[c]->delta_group_key();
            if (key == nullptr || *key == '\0') continue;
            auto [it, inserted] = group_by_key.try_emplace(key, candidates.size());
            if (inserted) {
                auto group = unit_constraints_[c]->create_delta_group();
                if (!group) {
                    throw std::logic_error("Delta group '" + std::string(key) + "' has no factory");
                }
                candidates.push_back(std::move(group));
            }
            candidates[it->second]->add(c, *unit_constraints_[c]);
        }

        for (auto& group : candidates) {
            if (group->indexes().size() < 2 || !group->is_beneficial()) continue;
            for (std::size_t index : group->indexes()) {
                if (index >= unit_constraints_.size() || grouped_delta_[index]) {
                    throw std::logic_error("Invalid or duplicate delta-group constraint index");
                }
                grouped_delta_[index] = 1;
            }
            group->on_reset(solution);
            delta_groups_.push_back(std::move(group));
        }
        refresh_direct_grouped_hard_deltas();
    }

    // The sequence shape traits calls are behind `if constexpr` so an assignment
    // solution (whose traits have no list operations) still compiles.
    int list_count_of(const Solution& solution) const {
        if constexpr (Traits::shape == Shape::Sequence) {
            return Traits::list_count(solution);
        } else {
            (void)solution;
            return 0;
        }
    }
    void reload_list_members(int list_index) {
        if constexpr (Traits::shape == Shape::Sequence) {
            const int size = Traits::list_size(*solution_, list_index);
            std::vector<int>& m = members_[list_index];
            m.resize(size);
            for (int p = 0; p < size; ++p) {
                m[p] = Traits::element_at(*solution_, list_index, p);
            }
        } else {
            (void)list_index;
        }
    }

    // Store a unit's new raw cost for one constraint (cell `idx` = unit row
    // base + entry.c) and fold the weighted, shaped (and, for hard, rounded)
    // difference into the running totals.
    void apply_raw(int unit, const UnitDispatchEntry& entry, std::size_t idx, double raw) {
        cache_raw_[idx] = raw;
        const double value = unit_contribution(entry, raw);
        const std::string& name = entry.constraint->name;
        if (entry.is_hard) {
            // Both cells are below 2^62 (rule 1), so their difference fits.
            const long long rounded = round_hard_contribution(value, name);
            hard_units_.add(rounded - cache_hard_[idx]);
            unit_hard_[unit].add(rounded - cache_hard_[idx]);
            cache_hard_[idx] = rounded;
        } else if (kMediumFold.has_medium() && entry.is_medium) {
            // Medium cells share cache_soft_ (a constraint has one level).
            const double medium = medium_contribution(value, name);
            medium_units_ += medium - cache_soft_[idx];
            cache_soft_[idx] = medium;
        } else {
            soft_units_ += checked_soft_contribution(value, name) - cache_soft_[idx];
            cache_soft_[idx] = value;
        }
    }

    // Re-cost one unit against its unit constraints up to `end`: dispatch_end
    // at a reset (every constraint), dispatch_live_end on a move.
    void refresh_unit(int unit, const UnitDispatchEntry* end) {
        const std::vector<int>& m = members_[unit];
        const std::size_t base = static_cast<std::size_t>(unit) * unit_constraints_.size();
        begin_share_pass();
        for (const UnitDispatchEntry* entry = dispatch_begin(unit); entry != end; ++entry) {
            apply_raw(unit, *entry, base + entry->c, raw_cost(*entry, unit, m));
        }
        set_penalized(unit, unit_hard_[unit].positive());
    }
    // A list edit: re-cost the unit, then notify the globals.
    void touch_unit(int unit) {
        refresh_unit(unit, dispatch_live_end(unit));
        for (GlobalConstraint<Solution>* global : notified_globals_) {
            global->on_unit_changed(*solution_, unit, members_[unit]);
        }
    }
    // assignment move path: `entity` just joined/left `unit` (members_ already
    // updated). Delta-capable constraints adjust the cached raw cost by the
    // marginal term; the rest fall back to a full unit re-cost.
    void touch_unit_member_changed(int unit, int entity, bool joined) {
        moved_since_reset_ = true;
        if (trial_active_ && !rollback_active_) save_unit_for_trial(unit);
        const std::vector<int>& m = members_[unit];
        const std::size_t base = static_cast<std::size_t>(unit) * unit_constraints_.size();
        for (UnitConstraintDeltaGroup<Solution>* group : delta_group_ptrs_) {
            group->evaluate(*solution_, unit, m, entity, grouped_delta_buffer_.data());
        }
        const double* const grouped_deltas = grouped_delta_buffer_.data();
        long long* const cache_hard = cache_hard_.data();
        const double sign = joined ? 1.0 : -1.0;
        // Hard totals are integral, so folding the loop's contributions
        // through one local accumulator is exact (unlike the soft doubles,
        // which keep their original accumulation order through apply_raw).
        HardTotal hard_acc;
        const UnitDispatchEntry* const end = dispatch_live_end(unit);
        begin_share_pass();
        for (const UnitDispatchEntry* entry = dispatch_begin(unit); entry != end; ++entry) {
            switch (entry->kind) {
                case kGroupedHardInt: {
                    // Grouped, hard, integral raw delta and weight: fold the
                    // weighted delta directly, no rounding/shaping needed.
                    // Branchless on purpose: a zero delta folds zeros, which
                    // beats an unpredictable data-dependent branch here.
                    // (The cell's cache_raw_ is intentionally left stale: for a
                    // grouped-hard constraint nothing reads it until the next
                    // reset rebuilds every cell from cost().) The cell stays
                    // below 2^62 (rule 1 of levels.hpp), so the cast and the
                    // two additions are defined.
                    const double weighted = entry->weight * (sign * grouped_deltas[entry->c]);
                    long long& cell = cache_hard[base + entry->c];
                    if (!(std::fabs(static_cast<double>(cell) + weighted) < kMaxHardContribution)) {
                        throw_contribution_overflow(entry->constraint->name,
                                                    static_cast<double>(cell) + weighted, true);
                    }
                    const long long score_delta = static_cast<long long>(weighted);
                    cell += score_delta;
                    hard_acc.add(score_delta);
                    break;
                }
                case kGroupedApply: {
                    // Branchless like the hard case: re-applying an unchanged
                    // raw folds an exact 0.0 into the soft total.
                    const std::size_t idx = base + entry->c;
                    apply_raw(unit, *entry, idx, cache_raw_[idx] + sign * grouped_deltas[entry->c]);
                    break;
                }
                case kVirtualDelta: {
                    const double d = entry->constraint->delta_cost(*solution_, unit, m, entity);
                    if (d == 0.0) break;
                    const std::size_t idx = base + entry->c;
                    apply_raw(unit, *entry, idx, cache_raw_[idx] + sign * d);
                    break;
                }
                case kFullRecost:
                    if (batch_depth_ == 0 && !rollback_active_) {
                        save_unit_for_trial(unit);
                        apply_raw(unit, *entry, base + entry->c,
                                  entry->constraint->cost(*solution_, unit, m));
                    }
                    break;
                case kSharedVirtualDelta: {
                    const double d = shared_value(*entry, [&] {
                        return entry->constraint->delta_cost(*solution_, unit, m, entity);
                    });
                    if (d == 0.0) break;
                    const std::size_t idx = base + entry->c;
                    apply_raw(unit, *entry, idx, cache_raw_[idx] + sign * d);
                    break;
                }
                case kSharedFullRecost:
                    if (batch_depth_ == 0 && !rollback_active_) {
                        save_unit_for_trial(unit);
                        apply_raw(unit, *entry, base + entry->c, raw_cost(*entry, unit, m));
                    }
                    break;
            }
        }
        if (!hard_acc.is_zero()) {
            hard_units_.add(hard_acc);
            unit_hard_[unit].add(hard_acc);
        }
        for (GlobalConstraint<Solution>* global : member_change_globals_) {
            global->on_member_changed(*solution_, unit, m, entity, joined);
        }
        if (batch_depth_ > 0) {
            if (!dirty_units_[unit]) {
                dirty_units_[unit] = 1;
                dirty_unit_list_.push_back(unit);
            }
            return;
        }
        if (!rollback_active_) set_penalized(unit, unit_hard_[unit].positive());
        for (GlobalConstraint<Solution>* global : unit_change_globals_) {
            global->on_unit_changed(*solution_, unit, m);
        }
    }
    // Previews are a sequence shape feature and need every global's cooperation.
    // The preview_* bodies behind this gate are compiled for every shape, so
    // they may only use what both shapes have (members_, no list Traits calls).
    bool previews_served() const {
        if (Traits::shape != Shape::Sequence || solution_ == nullptr) return false;
        for (std::size_t g = 0; g < global_constraints_.size(); ++g) {
            // A global at weight 0 is never asked (see preview_score): the
            // weight sampled at the last reset, the one the score uses.
            if (global_knobs(g).weight != 0.0 && !global_constraints_[g]->supports_preview()) {
                return false;
            }
        }
        return true;
    }

    // One previewed list: its candidate contents as pieces (empty pieces are
    // dropped as they are added). Fixed capacity keeps a preview free of heap
    // allocation: no move needs more than five pieces.
    struct PreviewRun {
        static constexpr int kCapacity = 6;
        int unit = -1;
        int count = 0;
        ListPiece pieces[kCapacity];

        void add(const ListPiece& piece) {
            if (piece.end <= piece.begin) return;
            assert(count < kCapacity);
            pieces[count++] = piece;
        }
        // Insert `piece` so that it starts at element `index` of the run,
        // splitting the piece that position falls inside (two pieces at most).
        void insert_at(int index, const ListPiece& piece) {
            if (piece.end <= piece.begin) return;
            assert(count + 2 <= kCapacity);
            int at = 0;
            int offset = 0;
            while (at < count && offset + pieces[at].size() <= index) {
                offset += pieces[at].size();
                ++at;
            }
            if (at < count && offset < index) {
                // Split pieces[at] at `index - offset` elements in. Only
                // forward pieces are ever split (the source list's remainder
                // in preview_segment_move).
                const ListPiece whole = pieces[at];
                assert(!whole.reversed);
                const int head = index - offset;
                ListPiece first = whole, second = whole;
                first.end = whole.begin + head;
                second.begin = whole.begin + head;
                for (int i = count; i > at + 1; --i) pieces[i] = pieces[i - 1];
                pieces[at] = first;
                pieces[at + 1] = second;
                ++count;
                ++at;
            }
            for (int i = count; i > at; --i) pieces[i] = pieces[i - 1];
            pieces[at] = piece;
            ++count;
        }
    };

    // Writes a run out as a plain member list.
    void materialize(const PreviewRun& run, std::vector<int>& out) const {
        out.clear();
        for (int i = 0; i < run.count; ++i) {
            const ListPiece& piece = run.pieces[i];
            if (piece.detached()) {
                out.push_back(piece.begin);
                continue;
            }
            const std::vector<int>& source = members_[piece.list];
            if (piece.reversed) {
                for (int k = piece.end; k-- > piece.begin;) out.push_back(source[k]);
            } else {
                out.insert(out.end(), source.begin() + piece.begin, source.begin() + piece.end);
            }
        }
    }

    // The score with each run's unit holding the run's contents: the cached
    // contributions of the touched units are swapped for a fresh cost of the
    // candidate contents, per cell, with the same weight / shaping / hard
    // rounding apply_raw() uses. Piecewise constraints answer from their sums;
    // a candidate list is written out the first time a constraint needs it,
    // and only that list.
    Score preview_score(const PreviewRun* runs, int count) const {
        PreviewedUnit changes[2];
        auto materialize_run = [&](int i) {
            if (changes[i].members != nullptr) return;
            materialize(runs[i], preview_scratch_[i]);
            changes[i].members = &preview_scratch_[i];
        };
        auto materialize_all = [&] {
            for (int i = 0; i < count; ++i) materialize_run(i);
        };
        for (int i = 0; i < count; ++i) {
            changes[i] = {runs[i].unit, nullptr, runs[i].pieces, runs[i].count};
        }
        HardTotal hard = hard_units_;
        double medium = medium_units_;
        double soft = soft_units_;
        for (int i = 0; i < count; ++i) {
            const int unit = changes[i].unit;
            const std::size_t base = static_cast<std::size_t>(unit) * unit_constraints_.size();
            // A constraint at weight 0 contributes nothing and is not asked.
            const UnitDispatchEntry* const end = dispatch_live_end(unit);
            // The candidate raw cost of one entry: its piecewise answer, or
            // cost() on the written-out list.
            auto candidate_raw = [&](const UnitDispatchEntry& entry) {
                double raw;
                if (!piecewise_[entry.c] ||
                    !entry.constraint->piecewise_cost(*solution_, unit, runs[i].pieces,
                                                      runs[i].count, raw)) {
                    materialize_run(i);  // this run only: the other may stay piecewise
                    raw = entry.constraint->cost(*solution_, unit, *changes[i].members);
                }
                return raw;
            };
            begin_share_pass();
            for (const UnitDispatchEntry* entry = dispatch_begin(unit); entry != end; ++entry) {
                const std::size_t idx = base + entry->c;
                const double raw =
                        is_shared(entry->kind)
                                ? shared_value(*entry, [&] { return candidate_raw(*entry); })
                                : candidate_raw(*entry);
                const double value = unit_contribution(*entry, raw);
                const std::string& name = entry->constraint->name;
                if (entry->is_hard) {
                    hard.add(round_hard_contribution(value, name) - cache_hard_[idx]);
                } else if (kMediumFold.has_medium() && entry->is_medium) {
                    medium += medium_contribution(value, name) - cache_soft_[idx];
                } else {
                    soft += checked_soft_contribution(value, name) - cache_soft_[idx];
                }
            }
        }
        for (const GlobalScoreEntry& entry : global_score_) {
            if (entry.weight == 0.0) continue;
            double raw;
            if (!entry.piecewise ||
                !entry.constraint->piecewise_preview_cost(*solution_, changes, count, raw)) {
                materialize_all();
                raw = entry.constraint->preview_cost(*solution_, changes, count);
            }
            accumulate(entry, global_contribution(entry, raw), hard, medium, soft);
        }
        return fold_score(hard, medium, soft);
    }

    void set_penalized(int unit, bool on) {
        const bool is = penalized_pos_[unit] >= 0;
        if (on == is) return;
        if (on) {
            penalized_pos_[unit] = static_cast<int>(penalized_.size());
            penalized_.push_back(unit);
        } else {
            const int pos = penalized_pos_[unit];
            const int last = penalized_.back();
            penalized_[pos] = last;
            penalized_pos_[last] = pos;
            penalized_.pop_back();
            penalized_pos_[unit] = -1;
        }
    }

    void save_unit_for_trial(int unit) {
        if (!trial_active_ || rollback_active_ || saved_units_[unit]) return;
        saved_units_[unit] = 1;
        saved_unit_list_.push_back(unit);
        const std::size_t base = static_cast<std::size_t>(unit) * unit_constraints_.size();
        const UnitDispatchEntry* const end = dispatch_live_end(unit);
        for (const UnitDispatchEntry* entry = dispatch_begin(unit); entry != end; ++entry) {
            const std::size_t idx = base + entry->c;
            saved_raw_[idx] = cache_raw_[idx];
            saved_hard_[idx] = cache_hard_[idx];
            saved_soft_[idx] = cache_soft_[idx];
        }
        saved_unit_hard_[unit] = unit_hard_[unit];
    }

    // Score-time view of one global constraint (metadata sampled at reset).
    struct GlobalScoreEntry {
        GlobalConstraint<Solution>* constraint;
        double weight;
        double exponent;
        bool is_hard;
        bool is_medium;
        bool piecewise;  // supports_piecewise_preview(), sampled at reset
        double offset;
        bool clip;
        bool idle;  // weight 0 and idle_at_weight_zero(): not notified, cost() not asked
    };

    // One global's shaped, weighted total (the knobs sampled at reset).
    static double global_contribution(const GlobalScoreEntry& entry, double raw) {
        return weighted_cost(entry.weight, raw, entry.exponent, entry.offset, entry.clip);
    }

    // The knobs of one instance as the score uses them: sampled at the last
    // reset (compile_unit_dispatch for a unit constraint, global_score_ for a
    // global), the component's own metadata for one added since or before any
    // reset. constraint_totals() and constraint_costs() report these, so they
    // explain score() even after a component was edited.
    struct SampledKnobs {
        double weight;
        double exponent;
        Level level;
        double offset;
        bool clip;
    };
    static SampledKnobs live_knobs(const ConstraintMetadata& constraint) {
        return {constraint.weight, constraint.exponent, constraint.level, constraint.offset,
                constraint.clip};
    }
    SampledKnobs unit_knobs(std::size_t c) const {
        return c < unit_knobs_.size() ? unit_knobs_[c] : live_knobs(*unit_constraints_[c]);
    }
    SampledKnobs global_knobs(std::size_t g) const {
        if (g >= global_score_.size() ||
            global_score_[g].constraint != global_constraints_[g].get()) {
            return live_knobs(*global_constraints_[g]);
        }
        const GlobalScoreEntry& entry = global_score_[g];
        const Level level = entry.is_hard     ? Level::Hard
                            : entry.is_medium ? Level::Medium
                                              : Level::Soft;
        return {entry.weight, entry.exponent, level, entry.offset, entry.clip};
    }

    std::unordered_map<std::string, std::unique_ptr<SharedConstraintContext>> shared_contexts_;
    std::vector<SharedConstraintContext*> shared_context_list_;  // the same, insertion order
    std::vector<std::unique_ptr<UnitConstraint<Solution>>> unit_constraints_;
    std::vector<std::unique_ptr<GlobalConstraint<Solution>>> global_constraints_;
    // Global dispatch partition (raw pointers, built at reset): the
    // member-change fast-path subscribers and the unit-diff rest.
    std::vector<GlobalConstraint<Solution>*> member_change_globals_;
    std::vector<GlobalConstraint<Solution>*> unit_change_globals_;
    std::vector<GlobalConstraint<Solution>*> notified_globals_;  // both, registration order
    std::vector<GlobalScoreEntry> global_score_;                 // all globals, registration order
    std::vector<std::unique_ptr<UnitConstraintDeltaGroup<Solution>>> delta_groups_;
    std::vector<UnitConstraintDeltaGroup<Solution>*> delta_group_ptrs_;  // dense view
    std::vector<UnitDispatchEntry> dispatch_;     // per unit: compiled entries, contiguous
    std::vector<std::size_t> dispatch_start_;     // unit -> [start, end) into dispatch_
    std::vector<std::size_t> dispatch_live_end_;  // unit -> end of its entries with a weight
    std::vector<char> delta_capable_;       // supports_delta() per unit constraint, cached at reset
    std::vector<double> unit_offset_;       // per unit constraint: offset, sampled at reset
    std::vector<char> unit_clip_;           // per unit constraint: clip, sampled at reset
    std::vector<SampledKnobs> unit_knobs_;  // per unit constraint: every knob, sampled at reset
    std::vector<char> piecewise_;           // supports_piecewise_preview() per unit constraint
    std::vector<char> grouped_delta_;       // delta is supplied by a compiled structural group
    std::vector<char> direct_grouped_hard_delta_;
    std::vector<char> integral_grouped_delta_;  // in a group with has_integral_raw_deltas()
    std::vector<double> grouped_delta_buffer_;
    bool delta_groups_dirty_ = true;
    bool has_full_recost_ = false;  // some dispatch entry is kFullRecost (compile_unit_dispatch)
    // Share classes ("Repeats share the raw cost"), per unit constraint:
    // the class, and per class the pass its value belongs to and the value.
    // Mutable because a preview (const) shares within its own passes; like
    // preview_scratch_, that makes an objective single-threaded.
    std::vector<std::uint32_t> share_class_;
    mutable std::vector<std::uint64_t> share_stamp_;
    mutable std::vector<double> share_value_;
    mutable std::uint64_t share_pass_ = 0;

    Solution* solution_ = nullptr;
    // A move was applied since the last reset: what the moves leave out (unit
    // constraints at weight 0, idle globals) no longer describes members_.
    bool moved_since_reset_ = false;
    int unit_count_ = 0;
    int pending_old_unit_ = -1;
    std::vector<std::vector<int>>
            members_;  // per unit: entities (assignment) / elements (sequence)
    mutable std::vector<int> preview_scratch_[2];  // sequence previews: candidate contents
    std::vector<int> entity_pos_;        // assignment: index of entity in its unit's members
    std::vector<double> cache_raw_;      // per (unit, constraint), raw (pre-weight/shape)
    std::vector<long long> cache_hard_;  // per (unit, constraint), weighted + rounded
    std::vector<double> cache_soft_;     // per (unit, constraint), weighted
    HardTotal hard_units_;               // exact, never wraps (levels.hpp)
    double medium_units_ = 0.0;          // medium penalties, BEFORE the fold
    double soft_units_ = 0.0;
    std::vector<HardTotal> unit_hard_;  // per unit: weighted hard total (> 0 when penalizing)
    std::vector<int> penalized_;        // units with unit_hard_ > 0 (unordered)
    std::vector<int> penalized_pos_;    // index in penalized_, or -1
    int batch_depth_ = 0;
    std::vector<char> dirty_units_;
    std::vector<int> dirty_unit_list_;
    bool trial_active_ = false;
    bool rollback_active_ = false;
    std::vector<char> saved_units_;
    std::vector<int> saved_unit_list_;
    std::vector<double> saved_raw_;
    std::vector<long long> saved_hard_;
    std::vector<double> saved_soft_;
    std::vector<HardTotal> saved_unit_hard_;
    HardTotal saved_hard_units_total_;
    double saved_medium_units_total_ = 0;
    double saved_soft_units_total_ = 0;
};

}  // namespace optimaru

#endif  // OPTIMARU_SCORE_DECOMPOSED_OBJECTIVE_HPP

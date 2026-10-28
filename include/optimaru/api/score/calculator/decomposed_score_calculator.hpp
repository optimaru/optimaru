#ifndef OPTIMARU_API_SCORE_CALCULATOR_DECOMPOSED_SCORE_CALCULATOR_HPP
#define OPTIMARU_API_SCORE_CALCULATOR_DECOMPOSED_SCORE_CALCULATOR_HPP

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Decomposed score calculation
// ---------------------------------------------------------------------------
// Cost contract generalized over both variable kinds:
// a constraint declares that its cost decomposes as a sum over "units", and
// only writes the per-unit evaluation — the calculator below turns that into
// a correct IncrementalScoreCalculator by caching per-unit contributions and
// recomputing only the units a move touched.
//
//   LIST kind:  unit = one list (a route); members = its elements in order.
//               A route-level recalculation hook.
//   BASIC kind: unit = one value (e.g. an employee); members = the entities
//               currently assigned to it (order NOT meaningful).
//
// Cost sign convention: positive = penalty, negative = reward,
// total score component = -(weight * cost). `isHard` routes the contribution
// into the hard component (rounded per unit contribution — hard costs should
// be integral) instead of soft.
//
// By default a touched unit is re-costed from scratch (correct for any
// cost()). A constraint that also implements the optional deltaCost() —
// the marginal cost of one member joining/leaving its unit — is maintained
// incrementally instead, which brings an assembly within a small constant
// factor of a fully hand-written calculator while keeping cost definitions
// one small class each — and per-constraint score totals for free.

// Sign-preserving power shaping: raise a cost's magnitude to `exponent`
// without flipping rewards (negative costs) into penalties. exponent == 1 is
// the identity; 2/3 make concentrated per-unit violations disproportionately
// expensive (a superlinear penalty curve).
inline double shapeCost(double cost, double exponent) {
    if (exponent == 1.0 || cost == 0.0) return cost;
    return cost < 0.0 ? -std::pow(-cost, exponent) : std::pow(cost, exponent);
}

// Calculator-owned, type-erased state shared by related constraints. The
// calculator advances the generation once per resetWorkingSolution(), letting
// a domain rebuild a common projection exactly once without process-global or
// thread-local caches.
struct SharedConstraintContext {
    virtual ~SharedConstraintContext() = default;
    std::uint64_t generation = 0;
    void beginReset() { ++generation; }

    // LIST kind: called by the calculator once per shared context when a list
    // has changed (after the change, before the constraints re-cost it) and
    // once per list at reset, after every constraint's onReset()/reset(). The
    // place for a per-list projection several components read, such as the
    // prefix sums behind a piecewise preview (see UnitConstraint::
    // piecewiseCost): one notification per change however many components
    // share it. The context reads the lists from the solution a constraint
    // handed it in onReset(). A reset follows every rejected compound move
    // (SolutionSnapshot::restore), so a context that rebuilds eagerly here
    // pays a full rebuild per reset; marking the list and rebuilding on the
    // first read is the cheaper design (see the VRP RouteSumsContext).
    virtual void onListChanged(int /*list*/) {}
};

template <typename Solution>
struct UnitConstraint;

// LIST kind: one run of a list as it is now, from which a previewed list is
// pieced together. The move kinds the engine previews rearrange runs of the
// existing lists, so a candidate list is at most five such pieces (an
// intra-list swap), plus one detached element for previewInsert.
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
// individual deltaCost() calls with one evaluate() call. The group must write
// one raw marginal cost to `deltas[index]` for every index it owns.
template <typename Solution>
struct UnitConstraintDeltaGroup {
    virtual ~UnitConstraintDeltaGroup() = default;
    virtual void add(std::size_t index, const UnitConstraint<Solution>& constraint) = 0;
    virtual const std::vector<std::size_t>& indexes() const = 0;
    virtual bool isBeneficial() const { return true; }
    virtual bool hasIntegralRawDeltas() const { return false; }
    virtual void onReset(const Solution& solution) = 0;
    virtual void evaluate(const Solution& solution, int unit, const std::vector<int>& members,
                          int entity, double* deltas) = 0;
};

// A cost that is a sum of independent per-unit terms.
template <typename Solution>
struct UnitConstraint {
    std::string name;
    double weight;
    bool isHard;
    // Applied per unit BEFORE the weight: contribution = weight *
    // shapeCost(cost, exponent). Mutable metadata, like weight/isHard: an
    // assembler may override it after construction.
    double exponent = 1.0;

    UnitConstraint(std::string constraintName, double constraintWeight, bool hard)
        : name(std::move(constraintName)), weight(constraintWeight), isHard(hard) {}
    virtual ~UnitConstraint() = default;

    // Called once per resetWorkingSolution, before any cost()/deltaCost().
    // Override to precompute lookup tables from the (prepared) solution —
    // e.g. per-(unit, entity) cost terms — so the per-move evaluations become
    // plain array reads. Default: nothing.
    virtual void onReset(const Solution& /*solution*/) {}

    // The unit's current cost, recomputed from scratch whenever the unit is
    // touched. Must not read state outside (solution, unit, members): in
    // particular no shadow variable of the solution, since a move preview
    // (LIST kind) evaluates candidate members BEFORE the move, when the
    // shadows still describe the old lists.
    virtual double cost(const Solution& solution, int unit,
                        const std::vector<int>& members) const = 0;

    // Optional incremental contract (BASIC kind): when supportsDelta() is
    // true, a member joining or leaving a unit is folded into the cached raw
    // cost as `raw ± deltaCost(...)` instead of re-running cost() over the
    // whole unit — O(members) or O(1) per move instead of O(members^2) for
    // pairwise costs. Shaping (exponent) and weight are applied on top of the
    // raw aggregate, so shaped constraints keep the fast path too.
    //
    // deltaCost returns the marginal raw cost of `entity` belonging to the
    // unit given the OTHER members: implementations iterating `members` must
    // skip `entity` itself, which may or may not be present (it already is
    // right after a join, no longer after a leave). The contract is exactness:
    //   cost(members ∪ {entity}) - cost(members \ {entity}) == deltaCost(...)
    // which holds whenever cost() is a sum of per-member and per-pair terms.
    virtual bool supportsDelta() const { return false; }
    virtual double deltaCost(const Solution& /*solution*/, int /*unit*/,
                             const std::vector<int>& /*members*/, int /*entity*/) const {
        return 0.0;
    }

    // Optional LIST-kind piecewise preview contract. The calculator serves the
    // engine's move previews (see IncrementalScoreCalculator) by re-costing
    // the touched lists on their candidate contents; by default it writes
    // the candidate list out and asks cost(), O(list) per constraint. A
    // constraint whose cost is a function of per-element quantities summed
    // along the list (load, service time, driving time between neighbours)
    // answers instead from prefix sums it keeps per list, in O(pieces):
    // piecewiseCost() receives the candidate list as runs of the current
    // lists (see ListPiece) and returns what cost() would return for the
    // written-out list. Return false to decline one call (a bound the sums
    // cannot settle): the calculator then materializes the list and asks
    // cost(). The sums live in a SharedConstraintContext refreshed through
    // its onListChanged() hook, so several components share one refresh.
    // FULL_ASSERT checks every preview against the applied score.
    virtual bool supportsPiecewisePreview() const { return false; }
    virtual bool piecewiseCost(const Solution& /*solution*/, int /*unit*/,
                               const ListPiece* /*pieces*/, int /*count*/,
                               double& /*out*/) const {
        return false;
    }

    // Optional unit-scope contract. Default nullptr = "may be non-zero for ANY
    // unit", so the calculator re-costs it on every touched unit. Return the
    // explicit list of unit indices it can contribute to when the constraint is
    // scoped to a subset (e.g. a per-worker rule): the calculator then visits it
    // only for those units and skips it everywhere else. The skip is exact ONLY
    // if cost()/deltaCost() are guaranteed 0 for every unit not in the list.
    virtual const std::vector<int>* scopedUnits() const { return nullptr; }

    // Optional structural grouping contract. Constraints returning the same
    // non-empty key are compiled into one delta evaluator when at least two
    // compatible constraints are present. Metadata (weight, hard/soft,
    // exponent) remains calculator-owned and therefore fully customizable.
    virtual const char* deltaGroupKey() const { return nullptr; }
    virtual std::unique_ptr<UnitConstraintDeltaGroup<Solution>> createDeltaGroup() const {
        return nullptr;
    }

    // Optional shared-context contract. Constraints returning the same key
    // receive one context owned by this calculator instance.
    virtual const char* sharedContextKey() const { return nullptr; }
    virtual std::unique_ptr<SharedConstraintContext> createSharedContext() const {
        return nullptr;
    }
    virtual void bindSharedContext(SharedConstraintContext& /*context*/) {}
};

// One list of a previewed move: the unit, its candidate contents as pieces of
// the current lists, and (for constraints without the piecewise contract) the
// same contents materialized. `members` is null while nothing needs it: the
// calculator materializes only when a constraint in the assembly lacks the
// piecewise contract or declines a piecewise answer.
struct PreviewedUnit {
    int unit;
    const std::vector<int>* members;
    const ListPiece* pieces;
    int pieceCount;
};

// A cost that does NOT decompose per unit (a statistic across units, e.g.
// load balancing). It maintains its own aggregates: notified once per touched
// unit, asked for the closed-form total at scoring time.
template <typename Solution>
struct GlobalConstraint {
    std::string name;
    double weight;
    bool isHard;
    double exponent = 1.0;  // same shaping as UnitConstraint::exponent

    GlobalConstraint(std::string constraintName, double constraintWeight, bool hard)
        : name(std::move(constraintName)), weight(constraintWeight), isHard(hard) {}
    virtual ~GlobalConstraint() = default;

    virtual void reset(const Solution& solution,
                       const std::vector<std::vector<int>>& membersPerUnit) = 0;
    virtual void onUnitChanged(const Solution& solution, int unit,
                               const std::vector<int>& members) = 0;
    // Optional BASIC-variable fast path. When enabled, the calculator sends
    // every join/leave directly (including inside a composite batch), avoiding
    // a full-unit diff/copy for global aggregates that can update in O(1).
    virtual bool supportsMemberChange() const { return false; }
    virtual void onMemberChanged(const Solution& solution, int unit,
                                 const std::vector<int>& members, int /*entity*/,
                                 bool /*joined*/) {
        onUnitChanged(solution, unit, members);
    }
    virtual double cost() const = 0;

    // Optional LIST-kind preview contract. The calculator serves the engine's
    // move previews (see IncrementalScoreCalculator) by re-costing the touched
    // lists on their candidate contents; a global constraint takes part by
    // answering what cost() WOULD be if each `changes[i].unit` held
    // `*changes[i].members` instead of its current members (at most two units
    // for the built-in moves), without mutating itself. The calculator serves
    // previews only when every global in the assembly supports them; one that
    // does not switches the whole assembly back to apply/score/undo.
    virtual bool supportsPreview() const { return false; }
    virtual double previewCost(const Solution& /*solution*/, const PreviewedUnit* /*changes*/,
                               int /*count*/) const {
        return cost();
    }
    // The piecewise form of the same answer (see UnitConstraint::
    // piecewiseCost): read `changes[i].pieces` instead of `*changes[i].members`,
    // which is null until some constraint needs it. Return false to fall back
    // to previewCost() on the materialized members for this one call.
    virtual bool supportsPiecewisePreview() const { return false; }
    virtual bool piecewisePreviewCost(const Solution& /*solution*/,
                                      const PreviewedUnit* /*changes*/, int /*count*/,
                                      double& /*out*/) const {
        return false;
    }

    // Optional shared-context contract, the same one UnitConstraint has:
    // constraints returning the same key receive one context owned by this
    // calculator instance. The typical use here is a projection several
    // globals read but none owns — e.g. a per-score recompute that all of
    // them select their own term out of.
    virtual const char* sharedContextKey() const { return nullptr; }
    virtual std::unique_ptr<SharedConstraintContext> createSharedContext() const {
        return nullptr;
    }
    virtual void bindSharedContext(SharedConstraintContext& /*context*/) {}
};

// One entry of the per-constraint score breakdown (the seed of a Timefold
// ScoreAnalysis / constraint-match view).
struct ConstraintTotal {
    std::string name;
    double cost;  // weighted, sign as defined above (positive = penalizing)
    bool isHard;
};

template <typename Solution>
class DecomposedScoreCalculator final : public IncrementalScoreCalculator<Solution> {
    using Descriptor = SolutionDescriptor<Solution>;

public:
    DecomposedScoreCalculator& add(std::unique_ptr<UnitConstraint<Solution>> constraint) {
        bindSharedContext(*constraint);
        unitConstraints_.push_back(std::move(constraint));
        deltaGroupsDirty_ = true;
        return *this;
    }
    DecomposedScoreCalculator& add(std::unique_ptr<GlobalConstraint<Solution>> constraint) {
        bindSharedContext(*constraint);
        globalConstraints_.push_back(std::move(constraint));
        return *this;
    }
    template <typename C, typename... Args>
    DecomposedScoreCalculator& emplace(Args&&... args) {
        return add(std::make_unique<C>(std::forward<Args>(args)...));
    }

    void resetWorkingSolution(Solution& workingSolution) override {
        solution_ = &workingSolution;
        if (profUnitNs_.empty() && !unitConstraints_.empty()) {
            prof_ = std::getenv("ES_PROFILE") != nullptr;
            profUnitNs_.assign(unitConstraints_.size(), 0);
            profUnitCalls_.assign(unitConstraints_.size(), 0);
            profUnitFull_.assign(unitConstraints_.size(), 0);
            profGlobalNs_.assign(globalConstraints_.size(), 0);
            profGlobalCalls_.assign(globalConstraints_.size(), 0);
        }
        if constexpr (Descriptor::kind == VariableKind::BASIC) {
            unitCount_ = Descriptor::valueCount(workingSolution);
            members_.assign(unitCount_, {});
            const int entityCount = Descriptor::entityCount(workingSolution);
            entityPos_.assign(entityCount, -1);
            for (int e = 0; e < entityCount; ++e) {
                int u = Descriptor::getVariable(workingSolution, e);
                if (u >= 0) {
                    entityPos_[e] = static_cast<int>(members_[u].size());
                    members_[u].push_back(e);
                }
            }
        } else {
            unitCount_ = listCountOf(workingSolution);
            members_.assign(unitCount_, {});
            for (int l = 0; l < unitCount_; ++l) reloadListMembers(l);
        }
        const std::size_t cells = static_cast<std::size_t>(unitCount_) * unitConstraints_.size();
        cacheRaw_.assign(cells, 0.0);
        cacheHard_.assign(cells, 0);
        cacheSoft_.assign(cells, 0.0);
        deltaCapable_.resize(unitConstraints_.size());
        hasNonDeltaConstraints_ = false;
        for (auto& [key, context] : sharedContexts_) {
            (void) key;
            context->beginReset();
        }
        // Preview capabilities, sampled once like the dispatch metadata. A
        // constraint at weight 0 (an opt-in objective left off) is not asked
        // during a preview at all: its contribution is 0 whatever it answers,
        // and some such costs are the most expensive in an assembly. The
        // applied path still costs it, since rawUnitCosts() reports it.
        piecewise_.resize(unitConstraints_.size());
        for (std::size_t c = 0; c < unitConstraints_.size(); ++c) {
            deltaCapable_[c] = unitConstraints_[c]->supportsDelta();
            hasNonDeltaConstraints_ |= !deltaCapable_[c];
            unitConstraints_[c]->onReset(workingSolution);
            piecewise_[c] = unitConstraints_[c]->supportsPiecewisePreview();
        }
        resetDeltaGroups(workingSolution);
        compileUnitDispatch();
        // Global dispatch partition: supportsMemberChange() is a capability
        // query (stable within one solve, like supportsDelta()/scopedUnits()),
        // so it is sampled once here instead of twice per member-change event.
        // Score-time metadata (weight/exponent/isHard) is sampled here too —
        // metadata overrides take effect at reset, when the cached per-unit
        // contributions are rebuilt against them anyway.
        memberChangeGlobals_.clear();
        memberChangeGlobalIdx_.clear();
        unitChangeGlobals_.clear();
        unitChangeGlobalIdx_.clear();
        globalScore_.clear();
        for (std::size_t g = 0; g < globalConstraints_.size(); ++g) {
            GlobalConstraint<Solution>* pointer = globalConstraints_[g].get();
            if (pointer->supportsMemberChange()) {
                memberChangeGlobals_.push_back(pointer);
                memberChangeGlobalIdx_.push_back(g);
            } else {
                unitChangeGlobals_.push_back(pointer);
                unitChangeGlobalIdx_.push_back(g);
            }
            globalScore_.push_back({pointer, pointer->weight, pointer->exponent, pointer->isHard,
                                    pointer->supportsPiecewisePreview()});
        }
        deltaGroupPtrs_.clear();
        for (const auto& group : deltaGroups_) deltaGroupPtrs_.push_back(group.get());
        hardUnits_ = 0;
        softUnits_ = 0.0;
        unitHard_.assign(unitCount_, 0);
        penalized_.clear();
        penalizedPos_.assign(unitCount_, -1);
        batchDepth_ = 0;
        dirtyUnits_.assign(unitCount_, 0);
        dirtyUnitList_.clear();
        trialActive_ = false;
        rollbackActive_ = false;
        savedUnits_.assign(unitCount_, 0);
        savedUnitList_.clear();
        savedRaw_.resize(cells);
        savedHard_.resize(cells);
        savedSoft_.resize(cells);
        savedUnitHard_.resize(unitCount_);
        for (int u = 0; u < unitCount_; ++u) refreshUnit(u);
        for (const auto& g : globalConstraints_) g->reset(workingSolution, members_);
        // Every constraint, unit and global, has seen the solution: the shared
        // contexts may now build whatever they keep per list.
        if constexpr (Descriptor::kind == VariableKind::LIST) {
            for (SharedConstraintContext* context : sharedContextList_) {
                for (int l = 0; l < unitCount_; ++l) context->onListChanged(l);
            }
        }
    }

    // BASIC: the entity leaves its old value's unit and joins the new one.
    // Delta-capable constraints fold the change into the cached raw cost;
    // the rest re-cost the touched unit from scratch.
    //
    // The bodies are behind `if constexpr` because these are virtuals: the
    // vtable instantiates them even for a LIST solution, whose descriptor has
    // no getVariable at all. The engine never calls this pair for a LIST kind.
    void beforeVariableChanged(int entityIndex) override {
        if constexpr (Descriptor::kind == VariableKind::BASIC) {
            pendingOldUnit_ = Descriptor::getVariable(*solution_, entityIndex);
        } else {
            (void)entityIndex;
        }
    }
    void afterVariableChanged(int entityIndex) override {
        if constexpr (Descriptor::kind != VariableKind::BASIC) {
            (void)entityIndex;
            return;
        } else {
        const int newUnit = Descriptor::getVariable(*solution_, entityIndex);
        if (newUnit == pendingOldUnit_) return;
        if (pendingOldUnit_ >= 0) {
            // Swap-remove via the tracked position (member order carries no
            // meaning for BASIC units).
            std::vector<int>& m = members_[pendingOldUnit_];
            const int pos = entityPos_[entityIndex];
            const int last = m.back();
            m[pos] = last;
            entityPos_[last] = pos;
            m.pop_back();
            entityPos_[entityIndex] = -1;
            touchUnitMemberChanged(pendingOldUnit_, entityIndex, /*joined=*/false);
        }
        if (newUnit >= 0) {
            entityPos_[entityIndex] = static_cast<int>(members_[newUnit].size());
            members_[newUnit].push_back(entityIndex);
            touchUnitMemberChanged(newUnit, entityIndex, /*joined=*/true);
        }
        }
    }

    void beginVariableChanges() override { ++batchDepth_; }

    void beginMoveEvaluation() override {
        // Delta-capable constraints restore themselves through the inverse
        // variable notifications.  Taking a per-unit transaction snapshot in
        // that case only duplicates work on every evaluated move.  Keep the
        // snapshot path for assemblies containing full re-cost constraints,
        // whose expensive rollback recalculation it is designed to avoid.
        if (!hasNonDeltaConstraints_) return;
        trialActive_ = true;
        rollbackActive_ = false;
        savedUnitList_.clear();
        savedHardUnitsTotal_ = hardUnits_;
        savedSoftUnitsTotal_ = softUnits_;
    }

    void beginMoveRollback() override {
        if (trialActive_) rollbackActive_ = true;
    }

    void endMoveEvaluation(bool accepted) override {
        if (!trialActive_) return;
        if (!accepted) {
            for (int unit : savedUnitList_) {
                const std::size_t base = static_cast<std::size_t>(unit) * unitConstraints_.size();
                const UnitDispatchEntry* entry = dispatch_.data() + dispatchStart_[unit];
                const UnitDispatchEntry* const end = dispatch_.data() + dispatchStart_[unit + 1];
                for (; entry != end; ++entry) {
                    const std::size_t idx = base + entry->c;
                    cacheRaw_[idx] = savedRaw_[idx];
                    cacheHard_[idx] = savedHard_[idx];
                    cacheSoft_[idx] = savedSoft_[idx];
                }
                unitHard_[unit] = savedUnitHard_[unit];
            }
            hardUnits_ = savedHardUnitsTotal_;
            softUnits_ = savedSoftUnitsTotal_;
            for (int unit : savedUnitList_) setPenalized(unit, unitHard_[unit] > 0);
        }
        for (int unit : savedUnitList_) savedUnits_[unit] = 0;
        savedUnitList_.clear();
        trialActive_ = false;
        rollbackActive_ = false;
    }

    void endVariableChanges() override {
        if (batchDepth_ <= 0 || --batchDepth_ != 0) return;
        for (int unit : dirtyUnitList_) {
            const std::vector<int>& members = members_[unit];
            if (!rollbackActive_) {
                if (hasNonDeltaConstraints_) {
                    const std::size_t base =
                            static_cast<std::size_t>(unit) * unitConstraints_.size();
                    const UnitDispatchEntry* entry = dispatch_.data() + dispatchStart_[unit];
                    const UnitDispatchEntry* const end =
                            dispatch_.data() + dispatchStart_[unit + 1];
                    for (; entry != end; ++entry) {
                        if (entry->kind != kFullRecost) continue;
                        saveUnitForTrial(unit);
                        applyRaw(unit, *entry, base + entry->c,
                                 entry->constraint->cost(*solution_, unit, members));
                    }
                }
                setPenalized(unit, unitHard_[unit] > 0);
            }
            for (GlobalConstraint<Solution>* global : unitChangeGlobals_) {
                global->onUnitChanged(*solution_, unit, members);
            }
            dirtyUnits_[unit] = 0;
        }
        dirtyUnitList_.clear();
    }

    // LIST: the touched list is re-read and re-costed whole (O(list size)).
    void beforeListChanged(int /*listIndex*/) override {}
    void afterListChanged(int listIndex) override {
        reloadListMembers(listIndex);
        for (SharedConstraintContext* context : sharedContextList_) {
            context->onListChanged(listIndex);
        }
        touchUnit(listIndex);
    }

    HardSoftScore calculateScore() override {
        long long hard = hardUnits_;
        double soft = softUnits_;
        for (const GlobalScoreEntry& entry : globalScore_) {
            const double c = entry.weight * shapeCost(entry.constraint->cost(), entry.exponent);
            if (entry.isHard) {
                hard += std::llround(c);
            } else {
                soft += c;
            }
        }
        // Normalize -0.0 to 0.0: a penalty-free assembly should report the
        // same "0soft" a hand-written calculator does, not "-0".
        return {-hard, soft == 0.0 ? 0.0 : -soft};
    }

    // Previews answer on a LIST domain whenever every global constraint can
    // preview (see GlobalConstraint::supportsPreview) and the working
    // solution is set.
    bool supportsPreviews() const override {
        if constexpr (Descriptor::kind != VariableKind::LIST) {
            return false;
        } else {
            return previewsServed();
        }
    }

    // --- LIST kind: move previews -----------------------------------------
    // Generic, for every assembly: the move's candidate list contents are
    // described as pieces of the current lists (see ListPiece) and every unit
    // constraint is asked what it would cost on them, exactly as
    // afterListChanged() would after the move, so the preview equals the
    // applied score by construction. A constraint with the piecewise contract
    // answers from its prefix sums in O(pieces); any other has the candidate
    // list written into a scratch vector and is asked cost() on it, O(list)
    // — for a cost() that keeps its contract and derives everything from
    // (solution facts, unit, members); one that read a shadow variable would
    // see the pre-move shadows here, which FULL_ASSERT reports. Either way a
    // preview is cheaper than the apply/score/undo cycle it replaces (no list
    // mutations, no second re-cost on undo, no shadow refreshes) and, more
    // importantly, lets the preview-only strategies (iterated local search)
    // run on an assembly at all. Global constraints take part through their
    // preview contract; an assembly with a global that lacks it declines
    // every preview (see GlobalConstraint::supportsPreview).
    bool previewSegmentMove(int fromList, int start, int len, int toList, int insertAt,
                            HardSoftScore& out) override {
        if constexpr (Descriptor::kind != VariableKind::LIST) {
            (void)fromList; (void)start; (void)len; (void)toList; (void)insertAt; (void)out;
            return false;
        } else {
            if (!previewsServed() || len <= 0) return false;
            const int fromSize = static_cast<int>(members_[fromList].size());
            const ListPiece segment{fromList, start, start + len, false};
            PreviewRun runs[2];
            runs[0].unit = fromList;
            runs[0].add({fromList, 0, start, false});
            runs[0].add({fromList, start + len, fromSize, false});
            if (fromList == toList) {
                runs[0].insertAt(insertAt, segment);
                out = previewScore(runs, 1);
                return true;
            }
            const int toSize = static_cast<int>(members_[toList].size());
            runs[1].unit = toList;
            runs[1].add({toList, 0, insertAt, false});
            runs[1].add(segment);
            runs[1].add({toList, insertAt, toSize, false});
            out = previewScore(runs, 2);
            return true;
        }
    }
    bool previewSwap(int l1, int p1, int l2, int p2, HardSoftScore& out) override {
        if constexpr (Descriptor::kind != VariableKind::LIST) {
            (void)l1; (void)p1; (void)l2; (void)p2; (void)out;
            return false;
        } else {
            if (!previewsServed()) return false;
            PreviewRun runs[2];
            if (l1 == l2) {
                if (p1 == p2) {  // a no-op: the current score, exactly
                    out = calculateScore();
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
                out = previewScore(runs, 1);
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
            out = previewScore(runs, 2);
            return true;
        }
    }
    bool previewReverse(int list, int first, int last, HardSoftScore& out) override {
        if constexpr (Descriptor::kind != VariableKind::LIST) {
            (void)list; (void)first; (void)last; (void)out;
            return false;
        } else {
            if (!previewsServed()) return false;
            const int size = static_cast<int>(members_[list].size());
            PreviewRun run;
            run.unit = list;
            run.add({list, 0, first, false});
            run.add({list, first, last + 1, true});
            run.add({list, last + 1, size, false});
            out = previewScore(&run, 1);
            return true;
        }
    }
    bool previewTailSwap(int l1, int c1, int l2, int c2, HardSoftScore& out) override {
        if constexpr (Descriptor::kind != VariableKind::LIST) {
            (void)l1; (void)c1; (void)l2; (void)c2; (void)out;
            return false;
        } else {
            if (!previewsServed() || l1 == l2) return false;
            const int size1 = static_cast<int>(members_[l1].size());
            const int size2 = static_cast<int>(members_[l2].size());
            PreviewRun runs[2];
            runs[0].unit = l1;
            runs[0].add({l1, 0, c1, false});
            runs[0].add({l2, c2, size2, false});
            runs[1].unit = l2;
            runs[1].add({l2, 0, c2, false});
            runs[1].add({l1, c1, size1, false});
            out = previewScore(runs, 2);
            return true;
        }
    }
    bool previewExchange(int l1, int p1, int len1, int l2, int p2, int len2,
                         HardSoftScore& out) override {
        if constexpr (Descriptor::kind != VariableKind::LIST) {
            (void)l1; (void)p1; (void)len1; (void)l2; (void)p2; (void)len2; (void)out;
            return false;
        } else {
            if (!previewsServed() || l1 == l2) return false;
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
            out = previewScore(runs, 2);
            return true;
        }
    }
    bool previewInsert(int list, int pos, int element, HardSoftScore& out) override {
        if constexpr (Descriptor::kind != VariableKind::LIST) {
            (void)list; (void)pos; (void)element; (void)out;
            return false;
        } else {
            if (!previewsServed()) return false;
            const int size = static_cast<int>(members_[list].size());
            PreviewRun run;
            run.unit = list;
            run.add({list, 0, pos, false});
            run.add({-1, element, element + 1, false});
            run.add({list, pos, size, false});
            out = previewScore(&run, 1);
            return true;
        }
    }

    // Weighted total per constraint, for score explanation ("which constraint
    // costs how much"). Cheap: sums the existing cache.
    std::vector<ConstraintTotal> constraintTotals() const {
        std::vector<ConstraintTotal> totals;
        const std::size_t C = unitConstraints_.size();
        for (std::size_t c = 0; c < C; ++c) {
            double sum = 0.0;
            for (int u = 0; u < unitCount_; ++u) {
                const std::size_t idx = static_cast<std::size_t>(u) * C + c;
                sum += unitConstraints_[c]->isHard ? static_cast<double>(cacheHard_[idx])
                                                   : cacheSoft_[idx];
            }
            totals.push_back({unitConstraints_[c]->name, sum, unitConstraints_[c]->isHard});
        }
        for (const auto& g : globalConstraints_) {
            totals.push_back({g->name, g->weight * shapeCost(g->cost(), g->exponent), g->isHard});
        }
        return totals;
    }

    // RAW per-unit costs per constraint — what cost() returned, before the
    // weight, the exponent and the hard rounding. constraintTotals() reports
    // the weighted result; this reports the input to it.
    //
    // It is what lets a caller re-price an assembly WITHOUT re-scoring: the
    // weight/level/exponent arithmetic is a pure function of these numbers
    // (see the Python packages' `score()`), so one scoring pass answers every
    // "what would this cost at weight 10 / as a soft term" question. The raw
    // cost has no weight baked in, so it stays valid as the metadata changes.
    //
    // Per-unit rather than summed because the exponent shapes each unit's cost
    // BEFORE the weight (see applyRaw): a different exponent cannot be
    // recovered from a total. Global constraints own no unit, so they report
    // their single cost() as a one-element list.
    struct ConstraintCosts {
        std::string name;
        double weight;   // the assembly's current weight (the domain default)
        double exponent;
        bool isHard;
        bool isGlobal;
        std::vector<double> unitCosts;
    };
    std::vector<ConstraintCosts> constraintCosts() const {
        std::vector<ConstraintCosts> out;
        const std::size_t C = unitConstraints_.size();
        out.reserve(C + globalConstraints_.size());
        for (std::size_t c = 0; c < C; ++c) {
            const UnitConstraint<Solution>& constraint = *unitConstraints_[c];
            std::vector<double> costs;
            costs.reserve(static_cast<std::size_t>(unitCount_));
            for (int u = 0; u < unitCount_; ++u) {
                costs.push_back(cacheRaw_[static_cast<std::size_t>(u) * C + c]);
            }
            out.push_back({constraint.name, constraint.weight, constraint.exponent,
                           constraint.isHard, false, std::move(costs)});
        }
        for (const auto& g : globalConstraints_) {
            out.push_back({g->name, g->weight, g->exponent, g->isHard, true, {g->cost()}});
        }
        return out;
    }

    const std::vector<std::vector<int>>& membersPerUnit() const { return members_; }
    std::size_t deltaGroupCount() const { return deltaGroups_.size(); }

    // Read-only view of the assembly, so a domain can inspect it (e.g. for
    // score explanation or configuration dumps).
    const std::vector<std::unique_ptr<UnitConstraint<Solution>>>& unitConstraintList() const {
        return unitConstraints_;
    }
    const std::vector<std::unique_ptr<GlobalConstraint<Solution>>>& globalConstraintList() const {
        return globalConstraints_;
    }

    // Units whose weighted hard cost is currently > 0, as an unordered list —
    // the generic analog of the hand-written calculator's penalizedEmployees(),
    // for violation-guided move selection. O(1) maintenance per touched unit.
    const std::vector<int>& penalizedUnits() const { return penalized_; }

private:
    // Hands `constraint` the context its key names, creating it on first use.
    // Shared by both add() overloads, so a unit and a global constraint asking
    // for the same key get the very same instance.
    template <typename Constraint>
    void bindSharedContext(Constraint& constraint) {
        const char* contextKey = constraint.sharedContextKey();
        if (contextKey == nullptr || *contextKey == '\0') return;
        auto [it, inserted] = sharedContexts_.try_emplace(contextKey);
        if (inserted) it->second = constraint.createSharedContext();
        if (!it->second) {
            throw std::logic_error("Constraint shared context '" + std::string(contextKey) +
                                   "' has no factory");
        }
        if (inserted) sharedContextList_.push_back(it->second.get());
        constraint.bindSharedContext(*it->second);
    }

    // How one (unit, constraint) pair is handled on the member-change hot
    // path, decided once per reset (capability queries and metadata are
    // sampled at resetWorkingSolution, like deltaCapable_ always was).
    enum DispatchKind : std::uint8_t {
        kGroupedHardInt = 0,  // grouped delta, hard, integral weight: direct fold
        kGroupedApply = 1,    // grouped delta folded through applyRaw (soft/shaped)
        kVirtualDelta = 2,    // per-constraint deltaCost() call
        kFullRecost = 3,      // no delta contract: full cost() re-run
    };
    // One compiled dispatch entry: everything the hot path needs, contiguous
    // (the constraint pointer for the virtual call, the classification, and
    // the reset-sampled metadata applyRaw uses).
    struct UnitDispatchEntry {
        UnitConstraint<Solution>* constraint;
        double weight;
        double exponent;
        std::uint32_t c;  // constraint index (cache/buffer addressing, prof rows)
        std::uint8_t kind;
        bool isHard;
    };

    // Compiles the per-unit dispatch table: one contiguous, pre-classified
    // entry per (unit, applicable constraint), so the per-event hot path is
    // one switch on `kind` instead of per-constraint capability probes and
    // scattered metadata loads. A unit visits every "applies-to-all"
    // constraint (scopedUnits()==nullptr) plus the ones explicitly scoped to
    // it. Constraints scoped to a subset of units are thereby skipped on the
    // ~all other units they can only contribute 0 to — the dominant waste
    // when many per-worker rules exist. All cost/save/restore loops iterate
    // this instead of [0,C), so scoped-out (unit,c) cells stay at their reset
    // value (0) forever, which is exactly their true cost.
    void compileUnitDispatch() {
        const std::size_t C = unitConstraints_.size();
        std::vector<UnitDispatchEntry> metas(C);
        for (std::size_t c = 0; c < C; ++c) {
            const UnitConstraint<Solution>& constraint = *unitConstraints_[c];
            UnitDispatchEntry& meta = metas[c];
            meta.constraint = unitConstraints_[c].get();
            meta.weight = constraint.weight;
            meta.exponent = constraint.exponent;
            meta.c = static_cast<std::uint32_t>(c);
            if (!deltaCapable_[c]) {
                meta.kind = kFullRecost;
            } else if (groupedDelta_[c]) {
                meta.kind = directGroupedHardDelta_[c] ? kGroupedHardInt : kGroupedApply;
            } else {
                meta.kind = kVirtualDelta;
            }
            meta.isHard = constraint.isHard;
        }
        std::vector<std::size_t> applyAll;
        for (std::size_t c = 0; c < C; ++c)
            if (unitConstraints_[c]->scopedUnits() == nullptr) applyAll.push_back(c);
        dispatchStart_.assign(static_cast<std::size_t>(unitCount_) + 1, 0);
        for (std::size_t c = 0; c < C; ++c) {
            const std::vector<int>* su = unitConstraints_[c]->scopedUnits();
            if (su == nullptr) continue;
            for (int u : *su)
                if (u >= 0 && u < unitCount_) ++dispatchStart_[u + 1];
        }
        std::size_t total = 0;
        for (int u = 0; u < unitCount_; ++u) {
            total += applyAll.size() + dispatchStart_[u + 1];
            dispatchStart_[u + 1] = total;
        }
        dispatch_.resize(total);
        std::vector<std::size_t> cursor(unitCount_);
        for (int u = 0; u < unitCount_; ++u) {
            std::size_t at = dispatchStart_[u];
            for (std::size_t c : applyAll) dispatch_[at++] = metas[c];
            cursor[u] = at;
        }
        for (std::size_t c = 0; c < C; ++c) {
            const std::vector<int>* su = unitConstraints_[c]->scopedUnits();
            if (su == nullptr) continue;
            for (int u : *su)
                if (u >= 0 && u < unitCount_) dispatch_[cursor[u]++] = metas[c];
        }
    }

    void resetDeltaGroups(const Solution& solution) {
        if (deltaGroupsDirty_) {
            rebuildDeltaGroups(solution);
            deltaGroupsDirty_ = false;
            return;
        }
        refreshDirectGroupedHardDeltas();
        for (const auto& group : deltaGroups_) group->onReset(solution);
    }

    void refreshDirectGroupedHardDeltas() {
        std::fill(directGroupedHardDelta_.begin(), directGroupedHardDelta_.end(), 0);
        for (const auto& group : deltaGroups_) {
            if (!group->hasIntegralRawDeltas()) continue;
            for (std::size_t index : group->indexes()) {
                const UnitConstraint<Solution>& constraint = *unitConstraints_[index];
                directGroupedHardDelta_[index] =
                        constraint.isHard && constraint.exponent == 1.0 &&
                        std::isfinite(constraint.weight) &&
                        std::trunc(constraint.weight) == constraint.weight;
            }
        }
    }

    void rebuildDeltaGroups(const Solution& solution) {
        deltaGroups_.clear();
        groupedDelta_.assign(unitConstraints_.size(), 0);
        directGroupedHardDelta_.assign(unitConstraints_.size(), 0);
        groupedDeltaBuffer_.assign(unitConstraints_.size(), 0.0);

        std::vector<std::unique_ptr<UnitConstraintDeltaGroup<Solution>>> candidates;
        std::unordered_map<std::string, std::size_t> groupByKey;
        for (std::size_t c = 0; c < unitConstraints_.size(); ++c) {
            if (!deltaCapable_[c]) continue;
            const char* key = unitConstraints_[c]->deltaGroupKey();
            if (key == nullptr || *key == '\0') continue;
            auto [it, inserted] = groupByKey.try_emplace(key, candidates.size());
            if (inserted) {
                auto group = unitConstraints_[c]->createDeltaGroup();
                if (!group) {
                    throw std::logic_error("Delta group '" + std::string(key) +
                                           "' has no factory");
                }
                candidates.push_back(std::move(group));
            }
            candidates[it->second]->add(c, *unitConstraints_[c]);
        }

        for (auto& group : candidates) {
            if (group->indexes().size() < 2 || !group->isBeneficial()) continue;
            for (std::size_t index : group->indexes()) {
                if (index >= unitConstraints_.size() || groupedDelta_[index]) {
                    throw std::logic_error("Invalid or duplicate delta-group constraint index");
                }
                groupedDelta_[index] = 1;
            }
            group->onReset(solution);
            deltaGroups_.push_back(std::move(group));
        }
        refreshDirectGroupedHardDeltas();
    }

    // The LIST-kind descriptor calls are behind `if constexpr` so a BASIC
    // solution (whose descriptor has no list operations) still compiles.
    int listCountOf(const Solution& solution) const {
        if constexpr (Descriptor::kind == VariableKind::LIST) {
            return Descriptor::listCount(solution);
        } else {
            (void)solution;
            return 0;
        }
    }
    void reloadListMembers(int listIndex) {
        if constexpr (Descriptor::kind == VariableKind::LIST) {
            const int size = Descriptor::listSize(*solution_, listIndex);
            std::vector<int>& m = members_[listIndex];
            m.resize(size);
            for (int p = 0; p < size; ++p) {
                m[p] = Descriptor::getElement(*solution_, listIndex, p);
            }
        } else {
            (void)listIndex;
        }
    }

    // Store a unit's new raw cost for one constraint (cell `idx` = unit row
    // base + entry.c) and fold the weighted, shaped (and, for hard, rounded)
    // difference into the running totals.
    void applyRaw(int unit, const UnitDispatchEntry& entry, std::size_t idx, double raw) {
        cacheRaw_[idx] = raw;
        const double value = entry.weight * shapeCost(raw, entry.exponent);
        if (entry.isHard) {
            const long long rounded = std::llround(value);
            hardUnits_ += rounded - cacheHard_[idx];
            unitHard_[unit] += rounded - cacheHard_[idx];
            cacheHard_[idx] = rounded;
        } else {
            softUnits_ += value - cacheSoft_[idx];
            cacheSoft_[idx] = value;
        }
    }

    // Re-cost one unit against every unit constraint, then notify the globals.
    void refreshUnit(int unit) {
        const std::vector<int>& m = members_[unit];
        const std::size_t base = static_cast<std::size_t>(unit) * unitConstraints_.size();
        const UnitDispatchEntry* entry = dispatch_.data() + dispatchStart_[unit];
        const UnitDispatchEntry* const end = dispatch_.data() + dispatchStart_[unit + 1];
        for (; entry != end; ++entry) {
            applyRaw(unit, *entry, base + entry->c,
                     entry->constraint->cost(*solution_, unit, m));
        }
        setPenalized(unit, unitHard_[unit] > 0);
    }
    void touchUnit(int unit) {
        refreshUnit(unit);
        for (const auto& g : globalConstraints_) {
            g->onUnitChanged(*solution_, unit, members_[unit]);
        }
    }
    // BASIC move path: `entity` just joined/left `unit` (members_ already
    // updated). Delta-capable constraints adjust the cached raw cost by the
    // marginal term; the rest fall back to a full unit re-cost. The body is
    // stamped out twice so the common Prof=false path carries no profiling
    // code at all (smaller hot code, no per-loop prof branches).
    void touchUnitMemberChanged(int unit, int entity, bool joined) {
        if (prof_) {
            touchUnitMemberChangedImpl<true>(unit, entity, joined);
        } else {
            touchUnitMemberChangedImpl<false>(unit, entity, joined);
        }
    }
    template <bool Prof>
    void touchUnitMemberChangedImpl(int unit, int entity, bool joined) {
        if (trialActive_ && !rollbackActive_) saveUnitForTrial(unit);
        const std::size_t C = unitConstraints_.size();
        const std::vector<int>& m = members_[unit];
        const std::size_t base = static_cast<std::size_t>(unit) * C;
        if constexpr (!Prof) {
            for (UnitConstraintDeltaGroup<Solution>* group : deltaGroupPtrs_) {
                group->evaluate(*solution_, unit, m, entity, groupedDeltaBuffer_.data());
            }
        } else {
            for (UnitConstraintDeltaGroup<Solution>* group : deltaGroupPtrs_) {
                const std::uint64_t t0 = nowNs_();
                group->evaluate(*solution_, unit, m, entity, groupedDeltaBuffer_.data());
                profGroupNs_ += nowNs_() - t0;
                ++profGroupCalls_;
            }
        }
        const UnitDispatchEntry* entry = dispatch_.data() + dispatchStart_[unit];
        const UnitDispatchEntry* const end = dispatch_.data() + dispatchStart_[unit + 1];
        const double* const groupedDeltas = groupedDeltaBuffer_.data();
        long long* const cacheHard = cacheHard_.data();
        const double sign = joined ? 1.0 : -1.0;
        // Hard totals are integral, so folding the loop's contributions
        // through one local accumulator is exact (unlike the soft doubles,
        // which keep their original accumulation order through applyRaw).
        long long hardAcc = 0;
        for (; entry != end; ++entry) {
            switch (entry->kind) {
                case kGroupedHardInt: {
                    // Grouped, hard, integral raw delta and weight: fold the
                    // weighted delta directly, no rounding/shaping needed.
                    // Branchless on purpose: a zero delta folds zeros, which
                    // beats an unpredictable data-dependent branch here.
                    // (The cell's cacheRaw_ is intentionally left stale: for a
                    // grouped-hard constraint nothing reads it until the next
                    // reset rebuilds every cell from cost().)
                    const long long scoreDelta = static_cast<long long>(
                            entry->weight * (sign * groupedDeltas[entry->c]));
                    cacheHard[base + entry->c] += scoreDelta;
                    hardAcc += scoreDelta;
                    break;
                }
                case kGroupedApply: {
                    // Branchless like the hard case: re-applying an unchanged
                    // raw folds an exact 0.0 into the soft total.
                    const std::size_t idx = base + entry->c;
                    applyRaw(unit, *entry, idx,
                             cacheRaw_[idx] + sign * groupedDeltas[entry->c]);
                    break;
                }
                case kVirtualDelta: {
                    double d;
                    if constexpr (Prof) {
                        const std::uint64_t t0 = nowNs_();
                        d = entry->constraint->deltaCost(*solution_, unit, m, entity);
                        profUnitNs_[entry->c] += nowNs_() - t0;
                        ++profUnitCalls_[entry->c];
                    } else {
                        d = entry->constraint->deltaCost(*solution_, unit, m, entity);
                    }
                    if (d == 0.0) break;
                    const std::size_t idx = base + entry->c;
                    applyRaw(unit, *entry, idx, cacheRaw_[idx] + sign * d);
                    break;
                }
                case kFullRecost:
                    if (batchDepth_ == 0 && !rollbackActive_) {
                        saveUnitForTrial(unit);
                        if constexpr (Prof) {
                            const std::uint64_t t0 = nowNs_();
                            const double v = entry->constraint->cost(*solution_, unit, m);
                            profUnitNs_[entry->c] += nowNs_() - t0;
                            ++profUnitCalls_[entry->c];
                            ++profUnitFull_[entry->c];
                            applyRaw(unit, *entry, base + entry->c, v);
                        } else {
                            applyRaw(unit, *entry, base + entry->c,
                                     entry->constraint->cost(*solution_, unit, m));
                        }
                    }
                    break;
            }
        }
        if (hardAcc != 0) {
            hardUnits_ += hardAcc;
            unitHard_[unit] += hardAcc;
        }
        if constexpr (!Prof) {
            for (GlobalConstraint<Solution>* global : memberChangeGlobals_) {
                global->onMemberChanged(*solution_, unit, m, entity, joined);
            }
        } else {
            for (std::size_t i = 0; i < memberChangeGlobals_.size(); ++i) {
                const std::uint64_t t0 = nowNs_();
                memberChangeGlobals_[i]->onMemberChanged(*solution_, unit, m, entity, joined);
                profGlobalNs_[memberChangeGlobalIdx_[i]] += nowNs_() - t0;
                ++profGlobalCalls_[memberChangeGlobalIdx_[i]];
            }
        }
        if (batchDepth_ > 0) {
            if (!dirtyUnits_[unit]) {
                dirtyUnits_[unit] = 1;
                dirtyUnitList_.push_back(unit);
            }
            return;
        }
        if (!rollbackActive_) setPenalized(unit, unitHard_[unit] > 0);
        if constexpr (!Prof) {
            for (GlobalConstraint<Solution>* global : unitChangeGlobals_) {
                global->onUnitChanged(*solution_, unit, m);
            }
        } else {
            for (std::size_t i = 0; i < unitChangeGlobals_.size(); ++i) {
                const std::uint64_t t0 = nowNs_();
                unitChangeGlobals_[i]->onUnitChanged(*solution_, unit, m);
                profGlobalNs_[unitChangeGlobalIdx_[i]] += nowNs_() - t0;
                ++profGlobalCalls_[unitChangeGlobalIdx_[i]];
            }
        }
    }
    // Previews are a LIST-kind feature and need every global's cooperation.
    bool previewsServed() const {
        if (solution_ == nullptr) return false;
        for (const auto& g : globalConstraints_) {
            // A global at weight 0 is never asked (see previewScore).
            if (g->weight != 0.0 && !g->supportsPreview()) return false;
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
        void insertAt(int index, const ListPiece& piece) {
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
                // in previewSegmentMove).
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
    // rounding applyRaw() uses. Piecewise constraints answer from their sums;
    // a candidate list is written out the first time a constraint needs it,
    // and only that list.
    HardSoftScore previewScore(const PreviewRun* runs, int count) const {
        PreviewedUnit changes[2];
        auto materializeRun = [&](int i) {
            if (changes[i].members != nullptr) return;
            materialize(runs[i], previewScratch_[i]);
            changes[i].members = &previewScratch_[i];
        };
        auto materializeAll = [&] {
            for (int i = 0; i < count; ++i) materializeRun(i);
        };
        for (int i = 0; i < count; ++i) {
            changes[i] = {runs[i].unit, nullptr, runs[i].pieces, runs[i].count};
        }
        long long hard = hardUnits_;
        double soft = softUnits_;
        for (int i = 0; i < count; ++i) {
            const int unit = changes[i].unit;
            const std::size_t base = static_cast<std::size_t>(unit) * unitConstraints_.size();
            const UnitDispatchEntry* entry = dispatch_.data() + dispatchStart_[unit];
            const UnitDispatchEntry* const end = dispatch_.data() + dispatchStart_[unit + 1];
            for (; entry != end; ++entry) {
                if (entry->weight == 0.0) continue;  // contributes nothing, see reset
                const std::size_t idx = base + entry->c;
                double raw;
                if (!piecewise_[entry->c] ||
                    !entry->constraint->piecewiseCost(*solution_, unit, runs[i].pieces,
                                                      runs[i].count, raw)) {
                    materializeRun(i);  // this run only: the other may stay piecewise
                    raw = entry->constraint->cost(*solution_, unit, *changes[i].members);
                }
                const double value = entry->weight * shapeCost(raw, entry->exponent);
                if (entry->isHard) {
                    hard += std::llround(value) - cacheHard_[idx];
                } else {
                    soft += value - cacheSoft_[idx];
                }
            }
        }
        for (const GlobalScoreEntry& entry : globalScore_) {
            if (entry.weight == 0.0) continue;
            double raw;
            if (!entry.piecewise ||
                !entry.constraint->piecewisePreviewCost(*solution_, changes, count, raw)) {
                materializeAll();
                raw = entry.constraint->previewCost(*solution_, changes, count);
            }
            const double c = entry.weight * shapeCost(raw, entry.exponent);
            if (entry.isHard) {
                hard += std::llround(c);
            } else {
                soft += c;
            }
        }
        return {-hard, soft == 0.0 ? 0.0 : -soft};
    }

    void setPenalized(int unit, bool on) {
        const bool is = penalizedPos_[unit] >= 0;
        if (on == is) return;
        if (on) {
            penalizedPos_[unit] = static_cast<int>(penalized_.size());
            penalized_.push_back(unit);
        } else {
            const int pos = penalizedPos_[unit];
            const int last = penalized_.back();
            penalized_[pos] = last;
            penalizedPos_[last] = pos;
            penalized_.pop_back();
            penalizedPos_[unit] = -1;
        }
    }

    void saveUnitForTrial(int unit) {
        if (!trialActive_ || rollbackActive_ || savedUnits_[unit]) return;
        savedUnits_[unit] = 1;
        savedUnitList_.push_back(unit);
        const std::size_t base = static_cast<std::size_t>(unit) * unitConstraints_.size();
        const UnitDispatchEntry* entry = dispatch_.data() + dispatchStart_[unit];
        const UnitDispatchEntry* const end = dispatch_.data() + dispatchStart_[unit + 1];
        for (; entry != end; ++entry) {
            const std::size_t idx = base + entry->c;
            savedRaw_[idx] = cacheRaw_[idx];
            savedHard_[idx] = cacheHard_[idx];
            savedSoft_[idx] = cacheSoft_[idx];
        }
        savedUnitHard_[unit] = unitHard_[unit];
    }

    // Score-time view of one global constraint (metadata sampled at reset).
    struct GlobalScoreEntry {
        GlobalConstraint<Solution>* constraint;
        double weight;
        double exponent;
        bool isHard;
        bool piecewise;  // supportsPiecewisePreview(), sampled at reset
    };

    std::unordered_map<std::string, std::unique_ptr<SharedConstraintContext>> sharedContexts_;
    std::vector<SharedConstraintContext*> sharedContextList_;  // the same, insertion order
    std::vector<std::unique_ptr<UnitConstraint<Solution>>> unitConstraints_;
    std::vector<std::unique_ptr<GlobalConstraint<Solution>>> globalConstraints_;
    // Global dispatch partition (raw pointers, built at reset): the
    // member-change fast-path subscribers and the unit-diff rest, plus the
    // original indices for the profiling rows.
    std::vector<GlobalConstraint<Solution>*> memberChangeGlobals_;
    std::vector<std::size_t> memberChangeGlobalIdx_;
    std::vector<GlobalConstraint<Solution>*> unitChangeGlobals_;
    std::vector<std::size_t> unitChangeGlobalIdx_;
    std::vector<GlobalScoreEntry> globalScore_;  // all globals, registration order
    std::vector<std::unique_ptr<UnitConstraintDeltaGroup<Solution>>> deltaGroups_;
    std::vector<UnitConstraintDeltaGroup<Solution>*> deltaGroupPtrs_;  // dense view
    std::vector<UnitDispatchEntry> dispatch_;  // per unit: compiled entries, contiguous
    std::vector<std::size_t> dispatchStart_;   // unit -> [start, end) into dispatch_
    std::vector<char> deltaCapable_;  // supportsDelta() per unit constraint, cached at reset
    std::vector<char> piecewise_;     // supportsPiecewisePreview() per unit constraint
    std::vector<char> groupedDelta_;  // delta is supplied by a compiled structural group
    std::vector<char> directGroupedHardDelta_;
    std::vector<double> groupedDeltaBuffer_;
    bool deltaGroupsDirty_ = true;
    bool hasNonDeltaConstraints_ = false;

    Solution* solution_ = nullptr;
    int unitCount_ = 0;
    int pendingOldUnit_ = -1;
    std::vector<std::vector<int>> members_;  // per unit: entities (BASIC) / elements (LIST)
    mutable std::vector<int> previewScratch_[2];  // LIST previews: candidate contents
    std::vector<int> entityPos_;             // BASIC: index of entity in its unit's members
    std::vector<double> cacheRaw_;           // per (unit, constraint), raw (pre-weight/shape)
    std::vector<long long> cacheHard_;       // per (unit, constraint), weighted + rounded
    std::vector<double> cacheSoft_;          // per (unit, constraint), weighted
    long long hardUnits_ = 0;
    double softUnits_ = 0.0;
    std::vector<long long> unitHard_;        // per unit: weighted hard total (>= 0 when penalizing)
    std::vector<int> penalized_;             // units with unitHard_ > 0 (unordered)
    std::vector<int> penalizedPos_;          // index in penalized_, or -1
    int batchDepth_ = 0;
    std::vector<char> dirtyUnits_;
    std::vector<int> dirtyUnitList_;
    bool trialActive_ = false;
    bool rollbackActive_ = false;
    std::vector<char> savedUnits_;
    std::vector<int> savedUnitList_;
    std::vector<double> savedRaw_;
    std::vector<long long> savedHard_;
    std::vector<double> savedSoft_;
    std::vector<long long> savedUnitHard_;
    long long savedHardUnitsTotal_ = 0;
    double savedSoftUnitsTotal_ = 0;

    // --- optional per-constraint hot-path profiling (env ES_PROFILE set) ---
    // Times the move-time dispatch: per unit-constraint deltaCost/full-cost, the
    // grouped-delta evaluate, and per-global onMember/onUnitChanged. Zero cost
    // when off (one predictable branch). Dumped to stderr on destruction.
    bool prof_ = false;
    mutable std::vector<std::uint64_t> profUnitNs_;      // per unit-constraint: total ns
    mutable std::vector<std::uint64_t> profUnitCalls_;   // per unit-constraint: delta+full calls
    mutable std::vector<std::uint64_t> profUnitFull_;    // per unit-constraint: full-recost calls (non-delta)
    mutable std::vector<std::uint64_t> profGlobalNs_;    // per global: total ns
    mutable std::vector<std::uint64_t> profGlobalCalls_; // per global: calls
    mutable std::uint64_t profGroupNs_ = 0;              // grouped-delta evaluate: total ns
    mutable std::uint64_t profGroupCalls_ = 0;

    static std::uint64_t nowNs_() {
        return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
    }

public:
    ~DecomposedScoreCalculator() {
        if (!prof_) return;
        struct Row {
            std::string name;
            std::uint64_t ns, calls, full;
            bool global;
        };
        std::vector<Row> rows;
        for (std::size_t c = 0; c < unitConstraints_.size(); ++c)
            rows.push_back({unitConstraints_[c]->name, profUnitNs_[c], profUnitCalls_[c], profUnitFull_[c], false});
        for (std::size_t g = 0; g < globalConstraints_.size(); ++g)
            rows.push_back({globalConstraints_[g]->name, profGlobalNs_[g], profGlobalCalls_[g], 0, true});
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ns > b.ns; });
        std::uint64_t tot = profGroupNs_;
        for (const auto& r : rows) tot += r.ns;
        std::fprintf(stderr, "\n=== ES_PROFILE: per-constraint move-time (total %.3f ms) ===\n", tot / 1e6);
        std::fprintf(stderr, "  grouped-delta evaluate: %.3f ms (%llu calls)\n", profGroupNs_ / 1e6,
                     (unsigned long long)profGroupCalls_);
        std::fprintf(stderr, "  %-34s %10s %12s %10s %8s %s\n", "constraint", "ms", "calls", "ns/call",
                     "full", "kind");
        for (const auto& r : rows) {
            if (r.calls == 0 && r.ns == 0) continue;
            std::fprintf(stderr, "  %-34s %10.3f %12llu %10.1f %8llu %s\n", r.name.c_str(), r.ns / 1e6,
                         (unsigned long long)r.calls, r.calls ? double(r.ns) / r.calls : 0.0,
                         (unsigned long long)r.full, r.global ? "global" : "unit");
        }
        std::fflush(stderr);
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_API_SCORE_CALCULATOR_DECOMPOSED_SCORE_CALCULATOR_HPP

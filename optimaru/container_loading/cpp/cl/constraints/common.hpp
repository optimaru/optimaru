#ifndef CL_CONSTRAINTS_COMMON_HPP
#define CL_CONSTRAINTS_COMMON_HPP

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cl/model/domain.hpp"
#include "cl/score/packing.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace cl {

// ---------------------------------------------------------------------------
// Plugin-style constraint components
// ---------------------------------------------------------------------------
// The thirteen container-loading constraints, one small component each on the
// engine's DecomposedObjective. This assembly is also the default path: a
// solve without a selection scores with defaultConstraints() (there is no
// hand-written incremental calculator beside it; ScoreCalculator is the full
// recompute the assembly is checked against).
//
// Unit choice: with the sequence shape the calculator's unit is one list,
// i.e. one container; members are its items IN LOADING ORDER. Ten of the
// thirteen costs are sums over the containers and so UnitConstraints.
// groupSplitCost, incompleteGroupCost and singleContainerCost span containers
// and are GlobalConstraints; the last two read the shared decodes too
// (PlacedGroupConstraint below).
//
// The unit constraints cost where the items stand, which is not in
// the solution: it is derived from the list by the decoder
// (cl/score/packing.hpp). They share one decode per (container, list) through
// PackingContext below, the engine's shared-context contract, so a move that
// touches a container decodes its candidate list once however many of them
// are in the assembly. The decode is a pure function of (problem facts,
// container, members), so each cost stays a pure function of (solution, unit,
// members) as the engine's contract requires; the context only remembers
// answers.
//
// The conventions every use case shares are described once: editing an
// assembly by name in optimaru/score/constraint_assembly.hpp; kDefaultWeight /
// kDefaultLevel, the registry names and the cost sign (positive = penalty, 0 =
// no violation) in docs/extending.md.

using DecomposedCalculator = optimaru::DecomposedObjective<ContainerLoadingProblem>;
using ConstraintAssembly = optimaru::ConstraintAssembly<ContainerLoadingProblem>;
using ClUnitConstraint = optimaru::UnitConstraint<ContainerLoadingProblem>;
using ClGlobalConstraint = optimaru::GlobalConstraint<ContainerLoadingProblem>;

// The decodes one calculator's constraints share (see PackingCache). The
// objective tells it when a list has changed; it then keeps that list's
// decode, which is the one the next candidates start from.
struct PackingContext final : optimaru::SharedConstraintContext {
    // Called by every constraint's on_reset(); the first call of a reset does
    // the work.
    void reset(const ContainerLoadingProblem& problem) {
        if (problem_ == &problem && seen_ == generation) return;
        problem_ = &problem;
        seen_ = generation;
        cache.reset(problem);
    }

    void on_list_changed(int list) override {
        if (problem_ == nullptr) return;
        cache.pin(list, problem_->containers[static_cast<std::size_t>(list)].items);
    }

    PackingCache cache;

private:
    // The solution the last reset was handed. The engine's shared-context
    // contract (SharedConstraintContext::on_list_changed) is that the lists
    // changed are those of the solution handed in on_reset(), and every
    // rebuild resets first, so this points at the solution being edited.
    // Only pinning reads it; a decode never does.
    const ContainerLoadingProblem* problem_ = nullptr;
    std::uint64_t seen_ = 0;
};

namespace detail {

// A unit constraint that costs where the items of a container stand.
struct PackedConstraint : ClUnitConstraint {
    using ClUnitConstraint::ClUnitConstraint;

    const char* shared_context_key() const final { return "cl.packing"; }
    std::unique_ptr<optimaru::SharedConstraintContext> create_shared_context() const final {
        return std::make_unique<PackingContext>();
    }
    void bind_shared_context(optimaru::SharedConstraintContext& context) final {
        context_ = static_cast<PackingContext*>(&context);
    }
    // A component that adds to this calls it first.
    void on_reset(const ContainerLoadingProblem& problem) override { context_->reset(problem); }

protected:
    // The decode of `members` as container `unit`'s loading order.
    const Packing& packing(const ContainerLoadingProblem& problem, int unit,
                           const std::vector<int>& members) const {
        return context_->cache.packing(problem, unit, members);
    }

private:
    PackingContext* context_ = nullptr;
};

// A global constraint on where the items of the groups a rule flags stand:
// per container and flagged group, how many of the group's items its list
// holds and how many of those found a place. The derived class says which
// groups count (flagged) and what a group costs from its totals (groupCost);
// the raw cost is the sum over the flagged groups. It reads the decodes the
// unit constraints share (PackingContext), so a preview asks the decode the
// unit constraints already made of the same candidate list.
struct GroupTally {
    int group = -1;
    int listed = 0;  // items of the group in the container's list
    int placed = 0;  // of those, the ones that found a place
};

struct PlacedGroupConstraint : ClGlobalConstraint {
    using ClGlobalConstraint::ClGlobalConstraint;

    const char* shared_context_key() const final { return "cl.packing"; }
    std::unique_ptr<optimaru::SharedConstraintContext> create_shared_context() const final {
        return std::make_unique<PackingContext>();
    }
    void bind_shared_context(optimaru::SharedConstraintContext& context) final {
        context_ = static_cast<PackingContext*>(&context);
    }

    // Whether group `group` counts, and what it costs from its totals: the
    // items listed, the items placed and the containers holding a placed one.
    virtual bool flagged(const ContainerLoadingProblem& problem, int group) const = 0;
    virtual std::int64_t groupCost(int listed, int placed, int holders) const = 0;

    // The raw cost of a whole solution from per-container tallies, for
    // ScoreCalculator.
    std::int64_t totalOf(const ContainerLoadingProblem& problem,
                         const std::vector<std::vector<GroupTally>>& tallies) const {
        const auto groups = static_cast<std::size_t>(problem.groupCount);
        std::vector<int> listed(groups, 0), placed(groups, 0), holders(groups, 0);
        for (const std::vector<GroupTally>& unit : tallies) {
            for (const GroupTally& tally : unit) {
                const auto g = static_cast<std::size_t>(tally.group);
                listed[g] += tally.listed;
                placed[g] += tally.placed;
                holders[g] += tally.placed > 0 ? 1 : 0;
            }
        }
        std::int64_t total = 0;
        for (std::size_t g = 0; g < groups; ++g) {
            if (flagged(problem, static_cast<int>(g)))
                total += groupCost(listed[g], placed[g], holders[g]);
        }
        return total;
    }

    // The tally of the flagged groups in one container's decoded list.
    void tally(const ContainerLoadingProblem& problem, const std::vector<int>& members,
               const Packing& packing, std::vector<GroupTally>& out) const {
        out.clear();
        for (std::size_t position = 0; position < members.size(); ++position) {
            const int group = problem.groupOf[static_cast<std::size_t>(members[position])];
            if (group < 0 || !flagged_[static_cast<std::size_t>(group)]) continue;
            GroupTally* found = nullptr;
            for (GroupTally& entry : out) {
                if (entry.group == group) found = &entry;
            }
            if (found == nullptr) {
                out.push_back({group, 0, 0});
                found = &out.back();
            }
            ++found->listed;
            if (packing.boxOf[position] >= 0) ++found->placed;
        }
    }

    void reset(const ContainerLoadingProblem& problem,
               const std::vector<std::vector<int>>& members_per_unit) override {
        context_->reset(problem);
        const auto groups = static_cast<std::size_t>(problem.groupCount);
        flagged_.assign(groups, 0);
        active_ = false;
        for (std::size_t g = 0; g < groups; ++g) {
            flagged_[g] = flagged(problem, static_cast<int>(g)) ? 1 : 0;
            active_ = active_ || flagged_[g];
        }
        tallies_.assign(members_per_unit.size(), {});
        listed_.assign(groups, 0);
        placed_.assign(groups, 0);
        holders_.assign(groups, 0);
        total_ = 0;
        if (!active_) return;
        for (std::size_t unit = 0; unit < members_per_unit.size(); ++unit) {
            const std::vector<int>& members = members_per_unit[unit];
            if (members.empty()) continue;
            tally(problem, members,
                  context_->cache.packing(problem, static_cast<int>(unit), members),
                  tallies_[unit]);
            for (const GroupTally& entry : tallies_[unit]) apply(entry, +1);
        }
        for (std::size_t g = 0; g < groups; ++g) {
            if (flagged_[g]) total_ += groupCost(listed_[g], placed_[g], holders_[g]);
        }
    }

    void on_unit_changed(const ContainerLoadingProblem& problem, int unit,
                         const std::vector<int>& members) override {
        if (!active_) return;
        std::vector<GroupTally>& held = tallies_[static_cast<std::size_t>(unit)];
        scratch_.clear();
        if (!members.empty())
            tally(problem, members, context_->cache.packing(problem, unit, members), scratch_);
        touched_.clear();
        for (const GroupTally& entry : held) touched_.push_back(entry.group);
        for (const GroupTally& entry : scratch_) touched_.push_back(entry.group);
        std::sort(touched_.begin(), touched_.end());
        touched_.erase(std::unique(touched_.begin(), touched_.end()), touched_.end());
        for (int group : touched_) total_ -= costOf(group);
        for (const GroupTally& entry : held) apply(entry, -1);
        for (const GroupTally& entry : scratch_) apply(entry, +1);
        for (int group : touched_) total_ += costOf(group);
        held.swap(scratch_);
    }

    double cost() const override { return static_cast<double>(total_); }

    // Nothing but cost() reads the tallies and reset() rebuilds them, so a
    // selection that leaves the constraint at weight 0 pays nothing.
    bool idle_at_weight_zero() const override { return true; }

    bool supports_preview() const override { return true; }
    double preview_cost(const ContainerLoadingProblem& problem,
                        const optimaru::PreviewedUnit* changes, int count) const override {
        if (!active_) return 0.0;
        // The changed units' tallies swapped for the candidates', on copies of
        // the totals of the groups they touch.
        delta_.clear();
        for (int i = 0; i < count; ++i) {
            const int unit = changes[i].unit;
            for (const GroupTally& entry : tallies_[static_cast<std::size_t>(unit)])
                shift(entry, -1);
            const std::vector<int>& members = *changes[i].members;
            if (members.empty()) continue;
            tally(problem, members, context_->cache.packing(problem, unit, members), scratch_);
            for (const GroupTally& entry : scratch_) shift(entry, +1);
        }
        std::int64_t total = total_;
        for (const Shift& change : delta_) {
            const auto g = static_cast<std::size_t>(change.group);
            total += groupCost(listed_[g] + change.listed, placed_[g] + change.placed,
                               holders_[g] + change.holders) -
                     groupCost(listed_[g], placed_[g], holders_[g]);
        }
        return static_cast<double>(total);
    }
    // Without a flagged group the cost is 0 whatever the lists are.
    bool supports_piecewise_preview() const override { return true; }
    bool piecewise_preview_cost(const ContainerLoadingProblem& /*problem*/,
                                const optimaru::PreviewedUnit* /*changes*/, int /*count*/,
                                double& out) const override {
        if (active_) return false;
        out = 0.0;
        return true;
    }

private:
    struct Shift {
        int group;
        int listed;
        int placed;
        int holders;
    };

    std::int64_t costOf(int group) const {
        const auto g = static_cast<std::size_t>(group);
        return groupCost(listed_[g], placed_[g], holders_[g]);
    }
    void apply(const GroupTally& entry, int sign) {
        const auto g = static_cast<std::size_t>(entry.group);
        listed_[g] += sign * entry.listed;
        placed_[g] += sign * entry.placed;
        holders_[g] += sign * (entry.placed > 0 ? 1 : 0);
    }
    void shift(const GroupTally& entry, int sign) const {
        Shift* found = nullptr;
        for (Shift& change : delta_) {
            if (change.group == entry.group) found = &change;
        }
        if (found == nullptr) {
            delta_.push_back({entry.group, 0, 0, 0});
            found = &delta_.back();
        }
        found->listed += sign * entry.listed;
        found->placed += sign * entry.placed;
        found->holders += sign * (entry.placed > 0 ? 1 : 0);
    }

    PackingContext* context_ = nullptr;
    std::vector<char> flagged_;
    bool active_ = false;
    std::vector<std::vector<GroupTally>> tallies_;  // per container, as last reported
    std::vector<int> listed_;
    std::vector<int> placed_;
    std::vector<int> holders_;
    std::int64_t total_ = 0;
    std::vector<int> touched_;
    mutable std::vector<GroupTally> scratch_;
    mutable std::vector<Shift> delta_;
};

}  // namespace detail

}  // namespace cl

#endif  // CL_CONSTRAINTS_COMMON_HPP

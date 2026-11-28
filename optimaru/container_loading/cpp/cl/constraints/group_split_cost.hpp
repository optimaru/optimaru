#ifndef CL_CONSTRAINTS_GROUP_SPLIT_COST_HPP
#define CL_CONSTRAINTS_GROUP_SPLIT_COST_HPP

#include <cstdint>
#include <utility>
#include <vector>

#include "cl/constraints/common.hpp"

namespace cl {

// Soft, weight 1: groupSplitCost -- a group (the items of one order) spread
// over several containers. Raw cost (global) = over the groups, the
// containers whose list holds one of its items, beyond the first; 0 = every
// group is in one container (or no item has a group).
//
// It counts where the lists put the items, placed or not: an item that found
// no place is an unplacedItemCost, and its group is no more split for it.
//
// Global because a group spans containers. It keeps, per group, how many of
// its items each container holds, updated from the list that changed, and
// answers the move previews from the candidate lists without changing those
// counts, so an assembly with this constraint still serves previews.
struct GroupSplitCost final : ClGlobalConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit GroupSplitCost(double weight = kDefaultWeight)
        : ClGlobalConstraint("groupSplitCost", weight, kDefaultLevel) {}

    // The raw cost of a whole solution, for ScoreCalculator.
    static std::int64_t splits(const ContainerLoadingProblem& problem) {
        std::vector<int> last(static_cast<std::size_t>(problem.groupCount), -1);
        std::vector<int> places(static_cast<std::size_t>(problem.groupCount), 0);
        for (std::size_t c = 0; c < problem.containers.size(); ++c) {
            for (int item : problem.containers[c].items) {
                const int group = problem.groupOf[static_cast<std::size_t>(item)];
                if (group < 0 || last[static_cast<std::size_t>(group)] == static_cast<int>(c))
                    continue;
                last[static_cast<std::size_t>(group)] = static_cast<int>(c);
                ++places[static_cast<std::size_t>(group)];
            }
        }
        std::int64_t total = 0;
        for (int count : places) total += count > 1 ? count - 1 : 0;
        return total;
    }

    void reset(const ContainerLoadingProblem& problem,
               const std::vector<std::vector<int>>& members_per_unit) override {
        const auto groups = static_cast<std::size_t>(problem.groupCount);
        held_.assign(groups, {});
        delta_.assign(groups, 0);
        stamp_.assign(groups, 0);
        inCandidate_.assign(groups, 0);
        counted_.assign(groups, 0);
        epoch_ = 0;
        mark_ = 0;
        total_ = 0;
        members_.assign(members_per_unit.size(), {});
        for (std::size_t unit = 0; unit < members_per_unit.size(); ++unit) {
            members_[unit] = members_per_unit[unit];
            for (int item : members_[unit]) add(problem, static_cast<int>(unit), item, +1);
        }
    }

    void on_unit_changed(const ContainerLoadingProblem& problem, int unit,
                         const std::vector<int>& members) override {
        std::vector<int>& held = members_[static_cast<std::size_t>(unit)];
        for (int item : held) add(problem, unit, item, -1);
        held = members;
        for (int item : held) add(problem, unit, item, +1);
    }

    double cost() const override { return static_cast<double>(total_); }

    // Nothing but cost() reads the counts and reset() rebuilds all of them,
    // so a selection that leaves the constraint at weight 0 pays nothing.
    bool idle_at_weight_zero() const override { return true; }

    bool supports_preview() const override { return true; }
    double preview_cost(const ContainerLoadingProblem& problem,
                        const optimaru::PreviewedUnit* changes, int count) const override {
        if (held_.empty()) return 0.0;
        ++epoch_;
        touched_.clear();
        for (int i = 0; i < count; ++i) {
            const int unit = changes[i].unit;
            ++mark_;
            for (int item : *changes[i].members) {
                const int group = problem.groupOf[static_cast<std::size_t>(item)];
                if (group >= 0) inCandidate_[static_cast<std::size_t>(group)] = mark_;
            }
            // Groups the container holds now and would no longer hold...
            for (int item : members_[static_cast<std::size_t>(unit)]) {
                const int group = problem.groupOf[static_cast<std::size_t>(item)];
                if (group < 0) continue;
                const auto g = static_cast<std::size_t>(group);
                if (inCandidate_[g] == mark_ || counted_[g] == mark_) continue;
                counted_[g] = mark_;
                change(group, -1);
            }
            // ...and groups it would newly hold.
            for (int item : *changes[i].members) {
                const int group = problem.groupOf[static_cast<std::size_t>(item)];
                if (group < 0) continue;
                const auto g = static_cast<std::size_t>(group);
                if (counted_[g] == mark_ || countOn(group, unit) > 0) continue;
                counted_[g] = mark_;
                change(group, +1);
            }
        }
        std::int64_t total = total_;
        for (int group : touched_) {
            const auto g = static_cast<std::size_t>(group);
            const int now = static_cast<int>(held_[g].size());
            total += beyondFirst(now + delta_[g]) - beyondFirst(now);
        }
        return static_cast<double>(total);
    }
    // A problem without groups costs nothing whatever the candidate lists
    // are, so its previews need none written out.
    bool supports_piecewise_preview() const override { return true; }
    bool piecewise_preview_cost(const ContainerLoadingProblem& /*problem*/,
                                const optimaru::PreviewedUnit* /*changes*/, int /*count*/,
                                double& out) const override {
        if (!held_.empty()) return false;
        out = 0.0;
        return true;
    }

private:
    static std::int64_t beyondFirst(int places) { return places > 1 ? places - 1 : 0; }

    int countOn(int group, int unit) const {
        for (const auto& [container, count] : held_[static_cast<std::size_t>(group)]) {
            if (container == unit) return count;
        }
        return 0;
    }

    // One item of a group joins (+1) or leaves (-1) a container's list.
    void add(const ContainerLoadingProblem& problem, int unit, int item, int sign) {
        const int group = problem.groupOf[static_cast<std::size_t>(item)];
        if (group < 0) return;
        auto& places = held_[static_cast<std::size_t>(group)];
        total_ -= beyondFirst(static_cast<int>(places.size()));
        bool found = false;
        for (std::size_t i = 0; i < places.size(); ++i) {
            if (places[i].first != unit) continue;
            found = true;
            places[i].second += sign;
            if (places[i].second == 0) {
                places[i] = places.back();
                places.pop_back();
            }
            break;
        }
        if (!found && sign > 0) places.emplace_back(unit, 1);
        total_ += beyondFirst(static_cast<int>(places.size()));
    }

    void change(int group, int by) const {
        const auto g = static_cast<std::size_t>(group);
        if (stamp_[g] != epoch_) {
            stamp_[g] = epoch_;
            delta_[g] = 0;
            touched_.push_back(group);
        }
        delta_[g] += by;
    }

    // group -> (container, how many of the group's items its list holds),
    // only the containers that hold some
    std::vector<std::vector<std::pair<int, int>>> held_;
    std::vector<std::vector<int>> members_;  // container -> its list as last reported
    std::int64_t total_ = 0;

    // Preview scratch, valid while its stamp is the current epoch or mark.
    mutable std::vector<int> delta_;
    mutable std::vector<std::uint64_t> stamp_;
    mutable std::vector<std::uint64_t> inCandidate_;
    mutable std::vector<std::uint64_t> counted_;
    mutable std::vector<int> touched_;
    mutable std::uint64_t epoch_ = 0;
    mutable std::uint64_t mark_ = 0;
};

}  // namespace cl

#endif  // CL_CONSTRAINTS_GROUP_SPLIT_COST_HPP

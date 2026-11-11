#ifndef PDP_CONSTRAINTS_REQUEST_GROUP_COST_HPP
#define PDP_CONSTRAINTS_REQUEST_GROUP_COST_HPP

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "pdp/constraints/common.hpp"

namespace pdp {

// Hard, weight 1000 (ScoreCalculator::kCouplingPenalty): requestGroupCost — the
// requests of one group (Delivery::groupIndex) ride one vehicle together, or
// all wait. Orders the input has merged into one job stay one job.
//
// A request is where its pickup is: on a vehicle, or on none (the unassigned
// pool). Raw cost (global) = over the groups, the requests that are not where
// most of their group is, so 0 = every group is whole (or nothing is grouped).
// A group split 2 + 1 costs 1, one spread over three places 2: every request
// that joins the others takes 1 off, which is the slope a stop-by-stop repair
// follows back to a whole group.
//
// Silent about a request whose own pickup and dropoff are apart:
// pairedRequestCost and sameVehicleCost price that.
struct RequestGroupCost final : PdpGlobalConstraint {
    static constexpr double kDefaultWeight = static_cast<double>(ScoreCalculator::kCouplingPenalty);
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit RequestGroupCost(double weight = kDefaultWeight)
        : PdpGlobalConstraint("requestGroupCost", weight, kDefaultLevel) {}

    void reset(const PdpRoutePlan& plan,
               const std::vector<std::vector<int>>& members_per_unit) override {
        // The pickups of the grouped requests, group by group. A group of one
        // has nobody to be apart from and is left out.
        std::vector<std::pair<int, int>> grouped;  // (group, pickup node)
        for (const Delivery& delivery : plan.deliveries) {
            if (delivery.groupIndex >= 0)
                grouped.emplace_back(delivery.groupIndex, delivery.pickupNodeIndex);
        }
        std::stable_sort(grouped.begin(), grouped.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        slotOf_.assign(plan.nodes.size(), -1);
        groupOf_.clear();
        start_.assign(1, 0);
        for (std::size_t first = 0; first < grouped.size();) {
            std::size_t last = first;
            while (last < grouped.size() && grouped[last].first == grouped[first].first) ++last;
            if (last - first > 1) {
                for (std::size_t i = first; i < last; ++i) {
                    slotOf_[static_cast<std::size_t>(grouped[i].second)] =
                            static_cast<int>(groupOf_.size());
                    groupOf_.push_back(static_cast<int>(start_.size()) - 1);
                }
                start_.push_back(static_cast<int>(groupOf_.size()));
            }
            first = last;
        }
        place_.assign(groupOf_.size(), -1);
        slotsOn_.assign(members_per_unit.size(), {});
        cost_.assign(start_.size() - 1, 0);
        touched_.assign(start_.size() - 1, 0);
        total_ = 0;
        if (groupOf_.empty()) return;

        for (std::size_t unit = 0; unit < members_per_unit.size(); ++unit) {
            if (detail::isPool(plan, static_cast<int>(unit))) continue;
            for (int nodeIndex : members_per_unit[unit]) {
                const int slot = slotOf_[static_cast<std::size_t>(nodeIndex)];
                if (slot < 0) continue;
                place_[static_cast<std::size_t>(slot)] = static_cast<int>(unit);
                slotsOn_[unit].push_back(slot);
            }
        }
        for (std::size_t group = 0; group + 1 < start_.size(); ++group) {
            cost_[group] = apart(static_cast<int>(group));
            total_ += cost_[group];
        }
    }

    void on_unit_changed(const PdpRoutePlan& plan, int unit,
                         const std::vector<int>& members) override {
        // The pool is "on no vehicle", which is what a pickup that left its
        // route already reads as.
        if (groupOf_.empty() || detail::isPool(plan, unit)) return;
        // Whatever was last recorded on this vehicle is nowhere until recorded
        // anew, unless another vehicle has recorded it since.
        std::vector<int>& recorded = slotsOn_[static_cast<std::size_t>(unit)];
        for (int slot : recorded) {
            if (place_[static_cast<std::size_t>(slot)] != unit) continue;
            place_[static_cast<std::size_t>(slot)] = -1;
            touch(groupOf_[static_cast<std::size_t>(slot)]);
        }
        recorded.clear();
        for (int nodeIndex : members) {
            const int slot = slotOf_[static_cast<std::size_t>(nodeIndex)];
            if (slot < 0) continue;
            place_[static_cast<std::size_t>(slot)] = unit;
            recorded.push_back(slot);
            touch(groupOf_[static_cast<std::size_t>(slot)]);
        }
        for (int group : touchedList_) {
            const auto g = static_cast<std::size_t>(group);
            total_ -= cost_[g];
            cost_[g] = apart(group);
            total_ += cost_[g];
            touched_[g] = 0;
        }
        touchedList_.clear();
    }

    double cost() const override { return static_cast<double>(total_); }

private:
    void touch(int group) {
        char& touched = touched_[static_cast<std::size_t>(group)];
        if (touched) return;
        touched = 1;
        touchedList_.push_back(group);
    }

    // The requests of a group that are not where most of it is.
    std::int64_t apart(int group) const {
        const auto first = static_cast<std::size_t>(start_[static_cast<std::size_t>(group)]);
        const auto last = static_cast<std::size_t>(start_[static_cast<std::size_t>(group) + 1]);
        scratch_.assign(place_.begin() + static_cast<std::ptrdiff_t>(first),
                        place_.begin() + static_cast<std::ptrdiff_t>(last));
        std::sort(scratch_.begin(), scratch_.end());
        std::size_t most = 0;
        for (std::size_t i = 0; i < scratch_.size();) {
            std::size_t j = i;
            while (j < scratch_.size() && scratch_[j] == scratch_[i]) ++j;
            most = std::max(most, j - i);
            i = j;
        }
        return static_cast<std::int64_t>(scratch_.size() - most);
    }

    std::vector<int> slotOf_;   // node -> its slot below, or -1 (not a grouped pickup)
    std::vector<int> groupOf_;  // slot -> its group, the groups numbered densely
    std::vector<int> start_;    // group -> its first slot; one past the last group at the end
    std::vector<int> place_;    // slot -> the vehicle the pickup rides, or -1
    // vehicle -> the slots last recorded on it, so a change costs what the
    // route holds and not every grouped request of the plan
    std::vector<std::vector<int>> slotsOn_;
    std::vector<std::int64_t> cost_;  // group -> its requests apart
    std::int64_t total_ = 0;

    std::vector<char> touched_;
    std::vector<int> touchedList_;
    mutable std::vector<int> scratch_;
};

}  // namespace pdp

#endif  // PDP_CONSTRAINTS_REQUEST_GROUP_COST_HPP

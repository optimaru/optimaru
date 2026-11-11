// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.
#ifndef OPTIMARU_SCORE_GROUP_PLACEMENT_HPP
#define OPTIMARU_SCORE_GROUP_PLACEMENT_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "optimaru/score/decomposed_objective.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// GroupPlacement
// ---------------------------------------------------------------------------
// The bookkeeping of a "these elements stay together" rule on a sequence
// solution: the elements of one group are meant to sit in one list together,
// or in none (the unassigned pool, or no list at all while a move is under
// way).
//
// It counts, over the groups, the elements that are not where most of their
// group is: 0 when every group is whole, 1 for a group of three with one
// element elsewhere, 2 when it is spread over three places. Every element that
// rejoins the others takes 1 off, which is the slope a search that moves one
// element at a time follows back to a whole group.
//
// A global constraint owns one and forwards reset(), on_unit_changed(), cost()
// and preview_cost() to it. A change costs what the changed list held and
// holds, not every grouped element, and a preview writes nothing.
class GroupPlacement {
public:
    // `group_of[element]` is the element's group, or -1 for none. A group of
    // one has nobody to be apart from and is left out. Lists 0 ..
    // counted_lists - 1 are places; any list past them stands for "nowhere".
    void reset(const std::vector<int>& group_of,
               const std::vector<std::vector<int>>& members_per_list, int counted_lists) {
        std::vector<std::pair<int, int>> grouped;  // (group, element)
        for (std::size_t element = 0; element < group_of.size(); ++element) {
            if (group_of[element] >= 0)
                grouped.emplace_back(group_of[element], static_cast<int>(element));
        }
        std::stable_sort(grouped.begin(), grouped.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        slot_of_.assign(group_of.size(), -1);
        group_of_slot_.clear();
        start_.assign(1, 0);
        for (std::size_t first = 0; first < grouped.size();) {
            std::size_t last = first;
            while (last < grouped.size() && grouped[last].first == grouped[first].first) ++last;
            if (last - first > 1) {
                for (std::size_t i = first; i < last; ++i) {
                    slot_of_[static_cast<std::size_t>(grouped[i].second)] =
                            static_cast<int>(group_of_slot_.size());
                    group_of_slot_.push_back(static_cast<int>(start_.size()) - 1);
                }
                start_.push_back(static_cast<int>(group_of_slot_.size()));
            }
            first = last;
        }
        counted_lists_ = counted_lists;
        place_.assign(group_of_slot_.size(), -1);
        slots_on_.assign(static_cast<std::size_t>(std::max(counted_lists, 0)), {});
        cost_.assign(start_.size() - 1, 0);
        touched_.assign(start_.size() - 1, 0);
        candidate_.assign(group_of_slot_.size(), -1);
        candidate_stamp_.assign(group_of_slot_.size(), 0);
        group_stamp_.assign(start_.size() - 1, 0);
        epoch_ = 0;
        total_ = 0;
        if (group_of_slot_.empty()) return;

        const std::size_t lists =
                std::min(members_per_list.size(), static_cast<std::size_t>(counted_lists));
        for (std::size_t list = 0; list < lists; ++list) {
            for (int element : members_per_list[list]) {
                const int slot = slot_of_[static_cast<std::size_t>(element)];
                if (slot < 0) continue;
                place_[static_cast<std::size_t>(slot)] = static_cast<int>(list);
                slots_on_[list].push_back(slot);
            }
        }
        for (std::size_t group = 0; group + 1 < start_.size(); ++group) {
            cost_[group] = apart_in(static_cast<int>(group), /*previewed=*/false);
            total_ += cost_[group];
        }
    }

    // Whether no group holds two elements: nothing to keep together.
    bool empty() const { return group_of_slot_.empty(); }

    // `list` now holds `members`. A list that is no place is ignored: what
    // left a place for it already reads as nowhere.
    void on_list_changed(int list, const std::vector<int>& members) {
        if (empty() || list < 0 || list >= counted_lists_) return;
        // Whatever was last recorded on this list is nowhere until recorded
        // anew, unless another list has recorded it since.
        std::vector<int>& recorded = slots_on_[static_cast<std::size_t>(list)];
        for (int slot : recorded) {
            if (place_[static_cast<std::size_t>(slot)] != list) continue;
            place_[static_cast<std::size_t>(slot)] = -1;
            touch(group_of_slot_[static_cast<std::size_t>(slot)]);
        }
        recorded.clear();
        for (int element : members) {
            const int slot = slot_of_[static_cast<std::size_t>(element)];
            if (slot < 0) continue;
            place_[static_cast<std::size_t>(slot)] = list;
            recorded.push_back(slot);
            touch(group_of_slot_[static_cast<std::size_t>(slot)]);
        }
        for (int group : touched_list_) {
            const auto g = static_cast<std::size_t>(group);
            total_ -= cost_[g];
            cost_[g] = apart_in(group, /*previewed=*/false);
            total_ += cost_[g];
            touched_[g] = 0;
        }
        touched_list_.clear();
    }

    // The elements that are not where most of their group is.
    std::int64_t apart() const { return total_; }

    // What apart() would be if each `changes[i].unit` held
    // `*changes[i].members` in place of what it holds.
    std::int64_t preview(const PreviewedUnit* changes, int count) const {
        if (empty()) return 0;
        ++epoch_;
        preview_groups_.clear();
        // What the previewed lists hold leaves them...
        for (int i = 0; i < count; ++i) {
            const int list = changes[i].unit;
            if (list < 0 || list >= counted_lists_) continue;
            for (int slot : slots_on_[static_cast<std::size_t>(list)]) {
                if (place_[static_cast<std::size_t>(slot)] == list) propose(slot, -1);
            }
        }
        // ...and their candidate members are on them.
        for (int i = 0; i < count; ++i) {
            const int list = changes[i].unit;
            if (list < 0 || list >= counted_lists_) continue;
            for (int element : *changes[i].members) {
                const int slot = slot_of_[static_cast<std::size_t>(element)];
                if (slot >= 0) propose(slot, list);
            }
        }
        std::int64_t total = total_;
        for (int group : preview_groups_) {
            total += apart_in(group, /*previewed=*/true) - cost_[static_cast<std::size_t>(group)];
        }
        return total;
    }

private:
    void touch(int group) {
        char& touched = touched_[static_cast<std::size_t>(group)];
        if (touched) return;
        touched = 1;
        touched_list_.push_back(group);
    }

    void propose(int slot, int place) const {
        const auto s = static_cast<std::size_t>(slot);
        candidate_[s] = place;
        candidate_stamp_[s] = epoch_;
        const int group = group_of_slot_[s];
        std::uint64_t& stamp = group_stamp_[static_cast<std::size_t>(group)];
        if (stamp == epoch_) return;
        stamp = epoch_;
        preview_groups_.push_back(group);
    }

    // The elements of a group that are not where most of it is, read from the
    // recorded places or, for the slots the current preview moved, from its
    // candidate places.
    std::int64_t apart_in(int group, bool previewed) const {
        const auto first = static_cast<std::size_t>(start_[static_cast<std::size_t>(group)]);
        const auto last = static_cast<std::size_t>(start_[static_cast<std::size_t>(group) + 1]);
        scratch_.clear();
        for (std::size_t slot = first; slot < last; ++slot) {
            scratch_.push_back(previewed && candidate_stamp_[slot] == epoch_ ? candidate_[slot]
                                                                             : place_[slot]);
        }
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

    std::vector<int> slot_of_;        // element -> its slot below, or -1 (in no group of two)
    std::vector<int> group_of_slot_;  // slot -> its group, the groups numbered densely
    std::vector<int> start_;          // group -> its first slot; one past the last group at the end
    std::vector<int> place_;          // slot -> the list the element sits in, or -1
    // list -> the slots last recorded on it, so a change costs what the list
    // holds and not every grouped element
    std::vector<std::vector<int>> slots_on_;
    std::vector<std::int64_t> cost_;  // group -> its elements apart
    std::int64_t total_ = 0;
    int counted_lists_ = 0;

    std::vector<char> touched_;
    std::vector<int> touched_list_;

    // Preview scratch: the places a preview proposes, valid while their stamp
    // is the current epoch.
    mutable std::vector<int> candidate_;
    mutable std::vector<std::uint64_t> candidate_stamp_;
    mutable std::vector<std::uint64_t> group_stamp_;
    mutable std::vector<int> preview_groups_;
    mutable std::uint64_t epoch_ = 0;
    mutable std::vector<int> scratch_;
};

}  // namespace optimaru

#endif  // OPTIMARU_SCORE_GROUP_PLACEMENT_HPP

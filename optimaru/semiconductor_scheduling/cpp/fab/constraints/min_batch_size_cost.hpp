#ifndef FAB_CONSTRAINTS_MIN_BATCH_SIZE_COST_HPP
#define FAB_CONSTRAINTS_MIN_BATCH_SIZE_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab {

// Soft, weight 1: minBatchSizeCost — lots missing below a batch tool's
// preferred minimum run size. Raw cost per batch machine: its assignments, in
// machine order, are grouped by (family, batch id) — the start time standing
// in for a missing or empty batch id — and each group adds
// max(0, minimum_batch_size - group size). 0 = every batch run is full
// enough.
struct MinBatchSizeCost final : ScheduleViewConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;
    static constexpr Term kTerm = Term::MinBatchSizeCost;

    explicit MinBatchSizeCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("minBatchSizeCost", weight, kDefaultLevel, kTerm) {}

    static std::int64_t machineCost(const SemiconductorSchedule& schedule, int machine,
                                    const PlacementList& rows, std::vector<std::int64_t>* items) {
        const Machine& m = schedule.machines[static_cast<std::size_t>(machine)];
        if (!m.isBatch()) return 0;
        // (family, key) -> size, in first-appearance order.
        std::vector<std::pair<std::pair<int, BatchKey>, std::int64_t>> groups;
        for (const ScorePlacement* p : rows) {
            const std::pair<int, BatchKey> key{terms::familyOf(schedule, p), p->batchKey};
            auto it = std::find_if(groups.begin(), groups.end(),
                                   [&](const auto& group) { return group.first == key; });
            if (it == groups.end())
                groups.push_back({key, 1});
            else
                ++it->second;
        }
        std::int64_t total = 0;
        for (const auto& group : groups)
            total = checked::add(total,
                                 terms::emit(items, std::max<std::int64_t>(
                                                            0, m.minimumBatchSize - group.second)));
        return total;
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_MIN_BATCH_SIZE_COST_HPP

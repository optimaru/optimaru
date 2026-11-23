#ifndef VRP_CONSTRAINTS_PROFILE_VISIT_TABLE_HPP
#define VRP_CONSTRAINTS_PROFILE_VISIT_TABLE_HPP

#include <algorithm>
#include <cstddef>
#include <vector>

#include "vrp/constraints/common.hpp"

namespace vrp {
namespace detail {

// ---------------------------------------------------------------------------
// ProfileVisitTable -- one flag per (profile entry, visit)
// ---------------------------------------------------------------------------
// What the two profile visit components count, as a table read in O(1) per
// visit: one row per entry of VehicleRoutePlan::profileVisits plus one for the
// vehicles that name none, one column per visit. Built at reset, from the
// plan's lists: O(rows * visits), the size of a memset at the instance sizes
// this use case runs. A plan without lists leaves it inactive, and the
// components then answer 0 without reading it.
//
// The previews hand the candidate route over as pieces of the current lists
// (optimaru::ListPiece); count() reads them in place, one table lookup per
// visit, so the candidate route is never written out for this component.
class ProfileVisitTable {
public:
    // kind: which flag a row holds.
    enum class Kind {
        Forbidden,  // the entry forbids the visit
        Unmatched,  // some vehicle's entry prefers the visit, this one does not
    };

    void build(const VehicleRoutePlan& plan, Kind kind) {
        plan_ = &plan;
        const std::size_t visits = plan.visits.size();
        const std::size_t entries = plan.profileVisits.size();
        active_ = kind == Kind::Forbidden ? plan.hasForbiddenVisits() : plan.hasPreferredVisits();
        if (!active_) return;
        visits_ = visits;
        rowOf_.resize(plan.vehicles.size());
        for (std::size_t v = 0; v < plan.vehicles.size(); ++v) {
            const int entry = plan.vehicles[v].profileVisitsIndex;
            rowOf_[v] = entry < 0 ? entries : static_cast<std::size_t>(entry);
        }
        flags_.assign((entries + 1) * visits, 0);
        if (kind == Kind::Forbidden) {
            for (std::size_t e = 0; e < entries; ++e) {
                for (int visit : plan.profileVisits[e].forbiddenVisitIndices)
                    flags_[e * visits + static_cast<std::size_t>(visit)] = 1;
            }
            return;
        }
        // Unmatched: the visits some vehicle's entry prefers, in every row,
        // then each entry's own preferences taken back out of its row.
        std::vector<char> referenced(entries, 0);
        for (const Vehicle& vehicle : plan.vehicles) {
            if (vehicle.profileVisitsIndex >= 0)
                referenced[static_cast<std::size_t>(vehicle.profileVisitsIndex)] = 1;
        }
        std::vector<char> wanted(visits, 0);
        for (std::size_t e = 0; e < entries; ++e) {
            if (!referenced[e]) continue;
            for (int visit : plan.profileVisits[e].preferredVisitIndices)
                wanted[static_cast<std::size_t>(visit)] = 1;
        }
        for (std::size_t row = 0; row <= entries; ++row) {
            std::copy(wanted.begin(), wanted.end(), flags_.begin() + row * visits);
        }
        for (std::size_t e = 0; e < entries; ++e) {
            for (int visit : plan.profileVisits[e].preferredVisitIndices)
                flags_[e * visits + static_cast<std::size_t>(visit)] = 0;
        }
    }

    bool active() const { return active_; }

    // The flagged visits among `members` on vehicle `vehicleIndex`.
    double count(int vehicleIndex, const std::vector<int>& members) const {
        const char* row = rowFor(vehicleIndex);
        int flagged = 0;
        for (int visit : members) flagged += row[visit];
        return static_cast<double>(flagged);
    }

    // The same for a candidate route given as pieces of the current lists.
    double count(int vehicleIndex, const optimaru::ListPiece* pieces, int pieceCount) const {
        const char* row = rowFor(vehicleIndex);
        int flagged = 0;
        for (int i = 0; i < pieceCount; ++i) {
            const optimaru::ListPiece& piece = pieces[i];
            if (piece.detached()) {
                flagged += row[piece.begin];
                continue;
            }
            const std::vector<int>& elements =
                    static_cast<std::size_t>(piece.list) >= plan_->vehicles.size()
                            ? plan_->unassignedVisitIndices
                            : plan_->vehicles[static_cast<std::size_t>(piece.list)].visitIndices;
            for (int k = piece.begin; k < piece.end; ++k)
                flagged += row[elements[static_cast<std::size_t>(k)]];
        }
        return static_cast<double>(flagged);
    }

private:
    const char* rowFor(int vehicleIndex) const {
        return flags_.data() + rowOf_[static_cast<std::size_t>(vehicleIndex)] * visits_;
    }

    const VehicleRoutePlan* plan_ = nullptr;
    bool active_ = false;
    std::size_t visits_ = 0;
    std::vector<std::size_t> rowOf_;  // per vehicle: its row
    std::vector<char> flags_;         // rows x visits
};

}  // namespace detail
}  // namespace vrp

#endif  // VRP_CONSTRAINTS_PROFILE_VISIT_TABLE_HPP

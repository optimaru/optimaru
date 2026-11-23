#ifndef PDP_CONSTRAINTS_PROFILE_REQUEST_TABLE_HPP
#define PDP_CONSTRAINTS_PROFILE_REQUEST_TABLE_HPP

#include <algorithm>
#include <cstddef>
#include <vector>

#include "pdp/constraints/common.hpp"

namespace pdp {
namespace detail {

// ---------------------------------------------------------------------------
// ProfileRequestTable -- one flag per (profile entry, stop)
// ---------------------------------------------------------------------------
// What the two profile request components count, as a table read in O(1) per
// stop: one row per entry of PdpRoutePlan::profileRequests plus one for the
// vehicles that name none, one column per node (both stops of a request carry
// its flag). Built at reset, from the plan's lists: O(rows * nodes). A plan
// without lists leaves it inactive, and the components then answer 0 without
// reading it.
//
// The previews hand the candidate route over as pieces of the current lists
// (optimaru::ListPiece); count() reads them in place, one lookup per stop.
class ProfileRequestTable {
public:
    enum class Kind {
        Forbidden,  // the entry forbids the stop's request
        Unmatched,  // some vehicle's entry prefers the stop's request, this one does not
    };

    void build(const PdpRoutePlan& plan, Kind kind) {
        plan_ = &plan;
        active_ =
                kind == Kind::Forbidden ? plan.hasForbiddenRequests() : plan.hasPreferredRequests();
        if (!active_) return;
        const std::size_t requests = plan.deliveries.size();
        const std::size_t entries = plan.profileRequests.size();
        nodes_ = plan.nodes.size();
        rowOf_.resize(plan.vehicles.size());
        for (std::size_t v = 0; v < plan.vehicles.size(); ++v) {
            const int entry = plan.vehicles[v].profileRequestsIndex;
            rowOf_[v] = entry < 0 ? entries : static_cast<std::size_t>(entry);
        }
        // Per request first, then spread over its two stops.
        std::vector<char> byRequest((entries + 1) * requests, 0);
        if (kind == Kind::Forbidden) {
            for (std::size_t e = 0; e < entries; ++e) {
                for (int d : plan.profileRequests[e].forbiddenRequestIndices)
                    byRequest[e * requests + static_cast<std::size_t>(d)] = 1;
            }
        } else {
            std::vector<char> referenced(entries, 0);
            for (const Vehicle& vehicle : plan.vehicles) {
                if (vehicle.profileRequestsIndex >= 0)
                    referenced[static_cast<std::size_t>(vehicle.profileRequestsIndex)] = 1;
            }
            std::vector<char> wanted(requests, 0);
            for (std::size_t e = 0; e < entries; ++e) {
                if (!referenced[e]) continue;
                for (int d : plan.profileRequests[e].preferredRequestIndices)
                    wanted[static_cast<std::size_t>(d)] = 1;
            }
            for (std::size_t row = 0; row <= entries; ++row)
                std::copy(wanted.begin(), wanted.end(), byRequest.begin() + row * requests);
            for (std::size_t e = 0; e < entries; ++e) {
                for (int d : plan.profileRequests[e].preferredRequestIndices)
                    byRequest[e * requests + static_cast<std::size_t>(d)] = 0;
            }
        }
        flags_.assign((entries + 1) * nodes_, 0);
        for (std::size_t row = 0; row <= entries; ++row) {
            for (std::size_t n = 0; n < nodes_; ++n) {
                flags_[row * nodes_ + n] =
                        byRequest[row * requests +
                                  static_cast<std::size_t>(plan.nodes[n].deliveryIndex)];
            }
        }
    }

    bool active() const { return active_; }

    // The flagged stops among `members` on vehicle `vehicleIndex`.
    double count(int vehicleIndex, const std::vector<int>& members) const {
        const char* row = rowFor(vehicleIndex);
        int flagged = 0;
        for (int node : members) flagged += row[node];
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
                            ? plan_->unassignedNodeIndices
                            : plan_->vehicles[static_cast<std::size_t>(piece.list)].nodeIndices;
            for (int k = piece.begin; k < piece.end; ++k)
                flagged += row[elements[static_cast<std::size_t>(k)]];
        }
        return static_cast<double>(flagged);
    }

private:
    const char* rowFor(int vehicleIndex) const {
        return flags_.data() + rowOf_[static_cast<std::size_t>(vehicleIndex)] * nodes_;
    }

    const PdpRoutePlan* plan_ = nullptr;
    bool active_ = false;
    std::size_t nodes_ = 0;
    std::vector<std::size_t> rowOf_;  // per vehicle: its row
    std::vector<char> flags_;         // rows x nodes
};

}  // namespace detail
}  // namespace pdp

#endif  // PDP_CONSTRAINTS_PROFILE_REQUEST_TABLE_HPP

// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_CHAIN_VIEW_HPP
#define OPTIMARU_CHAIN_VIEW_HPP

#include <algorithm>
#include <vector>

#include "optimaru/solution_traits.hpp"
#include "optimaru/derived_state.hpp"
#include "optimaru/chain_objective.hpp"
#include "optimaru/objective.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// ChainView
// ---------------------------------------------------------------------------
// The engine-internal sequence view of a chain-shaped solution: the chain of
// anchor a is list a. All search machinery runs on the lists; every list
// mutation is immediately written back to the domain's `previous` pointers
// (O(1) per insert/remove, O(segment) per reversal), so the domain model and
// any objective reading it stay consistent at every hook.
template <typename Domain>
struct ChainView {
    using ChainTraits = SolutionTraits<Domain>;

    Domain* domain;
    std::vector<std::vector<int>> lists;

    explicit ChainView(Domain& d) : domain(&d) { rebuild_from_pointers(); }

    // Decode the chains from the domain's previous pointers (unassigned
    // entities stay out of every list).
    void rebuild_from_pointers() {
        const int n = ChainTraits::entity_count(*domain);
        const int anchors = ChainTraits::anchor_count(*domain);
        lists.assign(anchors, {});
        std::vector<int> successor(n, -1);
        std::vector<int> head(anchors, -1);
        for (int e = 0; e < n; ++e) {
            int p = ChainTraits::previous_of(*domain, e);
            if (p == chain::kUnlinked) continue;
            if (chain::is_anchor(p)) {
                head[chain::anchor_index(p)] = e;
            } else {
                successor[p] = e;
            }
        }
        for (int a = 0; a < anchors; ++a) {
            for (int e = head[a]; e != -1; e = successor[e]) lists[a].push_back(e);
        }
    }
};

// The sequence traits over the chain view: list ops mutate the materialized
// lists AND resynchronize the affected previous pointers on the domain.
template <typename Domain>
struct SolutionTraits<ChainView<Domain>> {
    static constexpr Shape shape = Shape::Sequence;
    using CW = ChainView<Domain>;
    using ChainTraits = SolutionTraits<Domain>;

    static int list_count(const CW& s) { return static_cast<int>(s.lists.size()); }
    static int element_count(const CW& s) { return ChainTraits::entity_count(*s.domain); }
    static int list_size(const CW& s, int l) { return static_cast<int>(s.lists[l].size()); }
    static int element_at(const CW& s, int l, int p) { return s.lists[l][p]; }

    static void insert_element(CW& s, int l, int p, int e) {
        s.lists[l].insert(s.lists[l].begin() + p, e);
        sync_previous(s, l, p);
        if (p + 1 < static_cast<int>(s.lists[l].size())) sync_previous(s, l, p + 1);
    }
    static int remove_element(CW& s, int l, int p) {
        int e = s.lists[l][p];
        s.lists[l].erase(s.lists[l].begin() + p);
        ChainTraits::link(*s.domain, e, chain::kUnlinked);
        if (p < static_cast<int>(s.lists[l].size())) sync_previous(s, l, p);
        return e;
    }
    static void reverse_segment(CW& s, int l, int first, int last) {
        std::reverse(s.lists[l].begin() + first, s.lists[l].begin() + last + 1);
        for (int p = first; p <= last + 1 && p < static_cast<int>(s.lists[l].size()); ++p) {
            sync_previous(s, l, p);
        }
    }

private:
    static void sync_previous(CW& s, int l, int p) {
        int previous = p == 0 ? chain::anchor(l) : s.lists[l][p - 1];
        ChainTraits::link(*s.domain, s.lists[l][p], previous);
    }
};

// Bridges the user's ChainObjective into the inner sequence
// solve: list hooks become chain hooks carrying the chain's contents.
template <typename Domain>
class ChainObjectiveBridge final
    : public Objective<ChainView<Domain>> {
public:
    explicit ChainObjectiveBridge(ChainObjective<Domain>& inner)
        : inner_(inner) {}

    void rebuild(ChainView<Domain>& s) override {
        inner_.rebuild(*s.domain, s.lists);
        solution_ = &s;
    }
    void before_list_edit(int l) override {
        inner_.before_chain_edit(*solution_->domain, l, solution_->lists[l]);
    }
    void after_list_edit(int l) override {
        inner_.after_chain_edit(*solution_->domain, l, solution_->lists[l]);
    }
    Score score() override { return inner_.score(); }

private:
    ChainObjective<Domain>& inner_;
    ChainView<Domain>* solution_ = nullptr;
};

// Bridges domain-level derived state into the inner sequence solve.
template <typename Domain>
class ChainDerivedStateBridge final : public DerivedState<ChainView<Domain>> {
public:
    explicit ChainDerivedStateBridge(DerivedState<Domain>& inner) : inner_(inner) {}

    void rebuild(ChainView<Domain>& s) override {
        inner_.rebuild(*s.domain);
    }
    void after_list_edit(ChainView<Domain>& s, int l) override {
        inner_.after_list_edit(*s.domain, l);
    }

private:
    DerivedState<Domain>& inner_;
};

}  // namespace optimaru

#endif  // OPTIMARU_CHAIN_VIEW_HPP

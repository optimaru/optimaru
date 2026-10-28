#ifndef OPTIMARU_IMPL_DOMAIN_VARIABLE_CHAINED_WORKING_SOLUTION_HPP
#define OPTIMARU_IMPL_DOMAIN_VARIABLE_CHAINED_WORKING_SOLUTION_HPP

#include <algorithm>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/domain/variable/variable_listener.hpp"
#include "optimaru/api/score/calculator/chained_incremental_score_calculator.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// ChainedWorkingSolution
// ---------------------------------------------------------------------------
// The engine-internal LIST view of a CHAINED domain solution (mirrors what
// upstream's anchor/inverse shadow machinery derives from the chains): chain
// of anchor a == list a. All search machinery runs on the lists; every list
// mutation is immediately written back to the domain's `previous` pointers
// (O(1) per insert/remove, O(segment) per reversal), so the domain model and
// any calculator reading it stay consistent at every hook.
template <typename Domain>
struct ChainedWorkingSolution {
    using ChainedDescriptor = SolutionDescriptor<Domain>;

    Domain* domain;
    std::vector<std::vector<int>> lists;

    explicit ChainedWorkingSolution(Domain& d) : domain(&d) { rebuildFromPointers(); }

    // Decode the chains from the domain's previous pointers (unassigned
    // entities stay out of every list).
    void rebuildFromPointers() {
        const int n = ChainedDescriptor::entityCount(*domain);
        const int anchors = ChainedDescriptor::anchorCount(*domain);
        lists.assign(anchors, {});
        std::vector<int> successor(n, -1);
        std::vector<int> head(anchors, -1);
        for (int e = 0; e < n; ++e) {
            int p = ChainedDescriptor::getPrevious(*domain, e);
            if (p == chained::kUnassignedPrevious) continue;
            if (chained::isAnchor(p)) {
                head[chained::anchorIndex(p)] = e;
            } else {
                successor[p] = e;
            }
        }
        for (int a = 0; a < anchors; ++a) {
            for (int e = head[a]; e != -1; e = successor[e]) lists[a].push_back(e);
        }
    }
};

// The LIST descriptor over the chained view: list ops mutate the materialized
// lists AND resynchronize the affected previous pointers on the domain.
template <typename Domain>
struct SolutionDescriptor<ChainedWorkingSolution<Domain>> {
    static constexpr VariableKind kind = VariableKind::LIST;
    using CW = ChainedWorkingSolution<Domain>;
    using CD = SolutionDescriptor<Domain>;

    static int listCount(const CW& s) { return static_cast<int>(s.lists.size()); }
    static int elementCount(const CW& s) { return CD::entityCount(*s.domain); }
    static int listSize(const CW& s, int l) { return static_cast<int>(s.lists[l].size()); }
    static int getElement(const CW& s, int l, int p) { return s.lists[l][p]; }

    static void insertElement(CW& s, int l, int p, int e) {
        s.lists[l].insert(s.lists[l].begin() + p, e);
        syncPrevious(s, l, p);
        if (p + 1 < static_cast<int>(s.lists[l].size())) syncPrevious(s, l, p + 1);
    }
    static int removeElement(CW& s, int l, int p) {
        int e = s.lists[l][p];
        s.lists[l].erase(s.lists[l].begin() + p);
        CD::setPrevious(*s.domain, e, chained::kUnassignedPrevious);
        if (p < static_cast<int>(s.lists[l].size())) syncPrevious(s, l, p);
        return e;
    }
    static void reverseSegment(CW& s, int l, int first, int last) {
        std::reverse(s.lists[l].begin() + first, s.lists[l].begin() + last + 1);
        for (int p = first; p <= last + 1 && p < static_cast<int>(s.lists[l].size()); ++p) {
            syncPrevious(s, l, p);
        }
    }

private:
    static void syncPrevious(CW& s, int l, int p) {
        int previous = p == 0 ? chained::anchor(l) : s.lists[l][p - 1];
        CD::setPrevious(*s.domain, s.lists[l][p], previous);
    }
};

// Bridges the user's ChainedIncrementalScoreCalculator into the inner LIST
// solve: list hooks become chain hooks carrying the chain's contents.
template <typename Domain>
class ChainedCalculatorBridge final
    : public IncrementalScoreCalculator<ChainedWorkingSolution<Domain>> {
public:
    explicit ChainedCalculatorBridge(ChainedIncrementalScoreCalculator<Domain>& inner)
        : inner_(inner) {}

    void resetWorkingSolution(ChainedWorkingSolution<Domain>& s) override {
        inner_.resetWorkingSolution(*s.domain, s.lists);
        solution_ = &s;
    }
    void beforeListChanged(int l) override {
        inner_.beforeChainChanged(*solution_->domain, l, solution_->lists[l]);
    }
    void afterListChanged(int l) override {
        inner_.afterChainChanged(*solution_->domain, l, solution_->lists[l]);
    }
    HardSoftScore calculateScore() override { return inner_.calculateScore(); }

private:
    ChainedIncrementalScoreCalculator<Domain>& inner_;
    ChainedWorkingSolution<Domain>* solution_ = nullptr;
};

// Bridges domain-level shadow listeners into the inner LIST solve.
template <typename Domain>
class ChainedListenerBridge final : public VariableListener<ChainedWorkingSolution<Domain>> {
public:
    explicit ChainedListenerBridge(VariableListener<Domain>& inner) : inner_(inner) {}

    void resetWorkingSolution(ChainedWorkingSolution<Domain>& s) override {
        inner_.resetWorkingSolution(*s.domain);
    }
    void afterListChanged(ChainedWorkingSolution<Domain>& s, int l) override {
        inner_.afterListChanged(*s.domain, l);
    }

private:
    VariableListener<Domain>& inner_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_DOMAIN_VARIABLE_CHAINED_WORKING_SOLUTION_HPP

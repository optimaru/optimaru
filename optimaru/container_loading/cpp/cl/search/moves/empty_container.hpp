#ifndef CL_SEARCH_MOVES_EMPTY_CONTAINER_HPP
#define CL_SEARCH_MOVES_EMPTY_CONTAINER_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "cl/model/domain.hpp"
#include "cl/score/packing.hpp"
#include "cl/search/moves/list_edit.hpp"
#include "optimaru/search/operators.hpp"
#include "optimaru/utils/random.hpp"

namespace cl {

// ---------------------------------------------------------------------------
// EmptyContainerMove -- the domain's container-emptying macro move
// ---------------------------------------------------------------------------
// usedContainerCost is a step function of "does this container hold
// anything": a container stops costing only once its LAST item has left, so
// every single-item move on the way there keeps the score. This operator does
// the whole step in one move. It takes one of the `candidates` least filled
// containers in use (uniformly; random among equal fills) and moves its
// items, largest first (random among equal volumes), each to the fullest
// other container in use that still takes it: one whose weight limit holds
// and whose decode, with the item added where its unloading order asks,
// places the item, leaves no more items unplaced than before, and has no more
// weight above what an item carries, no more items in the way of an earlier
// stop and no more stacking rules or layer limits broken than before. (The unplaced count is
// compared, not the set: an item of the target may trade its place with one that had none. The hard
// score is the same either way.)
//
// "In use" here means the list is not empty, placed or not; the score's
// usedContainerCost counts only containers with a placed item. The acceptor
// judges every move, so the difference only widens what the move tries.
//
// Items no other container takes stay where they are. The move is then a
// partial one: the score usually stays, and the source is nearer to empty for
// the next try. When no item at all can move, one random item is relocated
// (relocateOneItem) so that the move is judged like any other.
//
// So the move adds no hard violation of its own; what it does to the groups
// and to the costs a selection adds is judged by the score, through the
// acceptor, like everything else.
//
// The move keeps decodes of its own (a PackingCache): a target is decoded
// once per move and then one added item at a time. Cost per move: a census of
// the lists, and for every item of the source a scan of the containers in
// use with one added item decoded per container tried.
class EmptyContainerMove final : public optimaru::MoveOperator<ContainerLoadingProblem> {
public:
    EmptyContainerMove(const ContainerLoadingProblem& problem, int candidates)
        : candidates_(std::max(1, candidates)) {
        cache_.reset(problem);
    }

    const char* name() const override { return "emptyContainer"; }

    bool apply(optimaru::OperatorContext<ContainerLoadingProblem>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        ContainerLoadingProblem& problem = ctx.editor.solution();
        const std::size_t containerCount = problem.containers.size();
        saved_.clear();

        // Census: the containers in use, with the volume and weight of their lists.
        // A list's volume counts its unplaced items too, so it is not bounded by
        // the container's and is summed in double (an int64 could overflow).
        used_.clear();
        volume_.assign(containerCount, 0.0);
        weight_.assign(containerCount, 0);
        for (std::size_t c = 0; c < containerCount; ++c) {
            const Container& container = problem.containers[c];
            if (container.items.empty()) continue;
            for (int item : container.items) {
                volume_[c] +=
                        static_cast<double>(problem.items[static_cast<std::size_t>(item)].volume());
                weight_[c] += problem.items[static_cast<std::size_t>(item)].weight;
            }
            used_.push_back({volume_[c] / static_cast<double>(container.volume()),
                             optimaru::random_index(rng, std::numeric_limits<int>::max()),
                             static_cast<int>(c)});
        }
        if (used_.size() < 2) return relocateOneItem(ctx, rng, entities, saved_);

        // The source: one of the k least filled. The rest, fullest first, are the targets.
        std::sort(used_.begin(), used_.end());
        const std::size_t k =
                std::min<std::size_t>(static_cast<std::size_t>(candidates_), used_.size());
        const int source =
                used_[static_cast<std::size_t>(optimaru::random_index(rng, static_cast<int>(k)))]
                        .container;

        members_ = problem.containers[static_cast<std::size_t>(source)].items;
        optimaru::random_shuffle(members_.begin(), members_.end(), rng);
        std::stable_sort(members_.begin(), members_.end(), [&problem](int a, int b) {
            return problem.items[static_cast<std::size_t>(a)].volume() >
                   problem.items[static_cast<std::size_t>(b)].volume();
        });

        if (plan_.size() < containerCount) plan_.resize(containerCount);
        planned_.assign(containerCount, 0);
        unplaced_.assign(containerCount, -1);
        excess_.assign(containerCount, 0);
        blocks_.assign(containerCount, 0);
        rules_.assign(containerCount, 0);
        moved_.clear();
        for (int item : members_) {
            const Item& box = problem.items[static_cast<std::size_t>(item)];
            for (auto target = used_.rbegin(); target != used_.rend(); ++target) {
                const int c = target->container;
                const auto index = static_cast<std::size_t>(c);
                if (c == source) continue;
                const Container& container = problem.containers[index];
                if (volume_[index] + static_cast<double>(box.volume()) >
                    static_cast<double>(container.volume()))
                    continue;
                if (container.maxWeight && weight_[index] + box.weight > *container.maxWeight)
                    continue;
                if (!planned_[index]) {
                    plan_[index] = container.items;
                    planned_[index] = 1;
                }
                if (unplaced_[index] < 0) {
                    const Packing& before = cache_.packing(problem, c, plan_[index]);
                    unplaced_[index] = before.unplaced();
                    excess_[index] = stackingExcess(problem, before);
                    blocks_[index] = unloadingBlocks(problem, before);
                    rules_[index] = stackingViolations(problem, before);
                }
                trial_ = plan_[index];
                const int position = insertPosition(problem, trial_, item);
                trial_.insert(trial_.begin() + position, item);
                const Packing& after = cache_.packing(problem, c, trial_);
                // The item itself has to find a place, and nothing else lose its own.
                if (after.boxOf[static_cast<std::size_t>(position)] < 0) continue;
                if (after.unplaced() > unplaced_[index]) continue;
                // Each kind of violation on its own: no trading one for the other.
                const std::int64_t excess = stackingExcess(problem, after);
                if (excess > excess_[index]) continue;
                const std::int64_t blocks = unloadingBlocks(problem, after);
                if (blocks > blocks_[index]) continue;
                const std::int64_t rules = stackingViolations(problem, after);
                if (rules > rules_[index]) continue;
                excess_[index] = excess;
                blocks_[index] = blocks;
                rules_[index] = rules;
                unplaced_[index] = after.unplaced();
                plan_[index].swap(trial_);
                volume_[index] += static_cast<double>(box.volume());
                weight_[index] += box.weight;
                moved_.push_back(item);
                break;
            }
        }
        if (moved_.empty()) return relocateOneItem(ctx, rng, entities, saved_);

        // The source keeps what did not move, in its order.
        remaining_.clear();
        for (int item : problem.containers[static_cast<std::size_t>(source)].items) {
            if (std::find(moved_.begin(), moved_.end(), item) == moved_.end())
                remaining_.push_back(item);
        }
        saved_.replace(ctx, source, remaining_);
        for (std::size_t c = 0; c < containerCount; ++c) {
            if (planned_[c] && plan_[c].size() != problem.containers[c].items.size())
                saved_.replace(ctx, static_cast<int>(c), plan_[c]);
        }
        entities.insert(entities.end(), moved_.begin(), moved_.end());
        return true;
    }

    void undo_last(optimaru::OperatorContext<ContainerLoadingProblem>& ctx) override {
        saved_.restore(ctx);
    }

private:
    struct Candidate {
        double fill;
        int tieBreak;
        int container;
        bool operator<(const Candidate& other) const {
            if (fill != other.fill) return fill < other.fill;
            return tieBreak < other.tieBreak;
        }
    };

    int candidates_;
    PackingCache cache_;
    SavedLists saved_;

    // Scratch, reused across moves.
    std::vector<Candidate> used_;
    std::vector<double> volume_;
    std::vector<std::int64_t> weight_;
    std::vector<int> members_;
    std::vector<std::vector<int>> plan_;
    std::vector<char> planned_;
    std::vector<int> unplaced_;
    std::vector<std::int64_t> excess_;  // per target: stacking excess of its plan
    std::vector<std::int64_t> blocks_;  // per target: unloading blocks of its plan
    std::vector<std::int64_t> rules_;   // per target: stacking rule violations of its plan
    std::vector<int> trial_;
    std::vector<int> moved_;
    std::vector<int> remaining_;
};

}  // namespace cl

#endif  // CL_SEARCH_MOVES_EMPTY_CONTAINER_HPP
